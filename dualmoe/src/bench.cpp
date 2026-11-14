#include "bench.h"

#include "gguf_inspect.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace dualmoe {

namespace {

// Same conservative constants the loader uses for its VRAM planner, so the
// benchmark's plan matches what load_model will actually accept.
constexpr uint64_t kComputeHeadroomMb = 360;

const char * kDefaultPrompt =
    "Ты — инженер по производительности локальных LLM. Объясни по пунктам, как "
    "гибридный движок VRAM ⊕ RAM распределяет экспертов MoE между видеокартой "
    "RX 580 и процессором Ryzen 7 5700X, и какие параметры влияют на скорость "
    "генерации токенов. Пиши развёрнуто, без списков кода.";

// Decode threads: half of the SMT count, but never fewer than all logical CPUs
// when the machine reports a small thread count (VMs / no-SMT hosts).
int32_t resolve_phys_threads(int32_t hw) {
    const char * env = std::getenv("DUALMOE_BENCH_HW_THREADS");
    if (env) {
        const int32_t forced = std::atoi(env);
        if (forced > 0) hw = forced;
    }
    if (hw <= 4) return hw;
    return std::max<int32_t>(1, hw / 2);
}

int64_t now_ms() {
    return (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string fmt(const char * f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

// KV-cache footprint for the GPU-resident part of the model (SWA + shared-KV
// aware) — mirrors the loader's estimator so the plan is consistent with it.
uint64_t estimate_kv_mb(const ModelSummary & m, const EngineConfig & cfg, int32_t ngl) {
    const int32_t total_L  = std::max<int32_t>(1, m.n_layer);
    const int32_t own_kv_L = std::max<int32_t>(1, total_L - std::max<int32_t>(0, m.n_kv_shared_layers));
    if (ngl <= 0) return 0;
    const int32_t first_gpu  = ngl >= total_L ? 0 : std::max<int32_t>(0, total_L - ngl);
    const int32_t gpu_own_kv = std::max<int32_t>(0, own_kv_L - first_gpu);
    if (gpu_own_kv <= 0) return 0;

    const uint64_t kv_elem_b = (cfg.kv_type_k == "f16" ? 2ull : 1ull) + (cfg.kv_type_v == "f16" ? 2ull : 1ull);
    const uint32_t ctx = cfg.n_ctx > 0 ? cfg.n_ctx : 8192;

    uint64_t bytes = 0;
    if (m.kv_dim_full > 0) {
        const int32_t swa_L = (m.n_swa > 0)
            ? std::min<int32_t>(gpu_own_kv, (int32_t) ((int64_t) m.n_swa_layers * gpu_own_kv / own_kv_L))
            : 0;
        const int32_t full_L = std::max<int32_t>(0, gpu_own_kv - swa_L);
        const uint64_t swa_ctx = std::min<uint64_t>(ctx, (uint64_t) m.n_swa + 128ull);
        bytes += (uint64_t) full_L * ctx * (uint64_t) m.kv_dim_full * kv_elem_b;
        bytes += (uint64_t) swa_L * swa_ctx * (uint64_t) std::max<int32_t>(1, m.kv_dim_swa) * kv_elem_b;
    } else {
        const uint64_t kv_dim_est = std::max<uint64_t>(256, (uint64_t) std::max<int32_t>(512, m.n_embd) / 3);
        bytes = (uint64_t) gpu_own_kv * ctx * kv_dim_est * kv_elem_b;
    }
    return (bytes + (1ull << 20) - 1) >> 20;
}

// Rough per-layer weight traffic for the benchmark prompt: the projected decode
// cost model only needs relative ordering between candidates, which the实测
// pass then confirms on real silicon.
double projected_decode_tps(const BenchCandidate & cand, int32_t hw_threads) {
    // Threads: linear up to the physical core count, ~0 past it (SMT thrash).
    const double th = std::clamp<double>((double) cand.cfg.n_threads / std::max(1, hw_threads), 0.10, 1.0);
    // VRAM bank: hot experts served from GDDR5 instead of DDR4.
    const double vram_span = std::clamp<double>((double) cand.vram_used_mb / 4600.0, 0.05, 1.0);
    // ubatch: bigger microbatches amortise the router/attention launches.
    const double ub = std::clamp<double>((double) cand.cfg.n_ubatch / 32.0, 0.35, 1.0);
    return 4.0 + 22.0 * th * (0.45 + 0.55 * vram_span) * (0.75 + 0.25 * ub);
}

} // namespace

BenchmarkRunner::BenchmarkRunner(DualMoeEngine & engine) : engine_(engine) {}

BenchmarkRunner::~BenchmarkRunner() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

BenchmarkSnapshot BenchmarkRunner::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return snap_;
}

bool BenchmarkRunner::best_config(EngineConfig & out) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!snap_.recommendation.ok) return false;
    out = snap_.recommendation.cfg;
    return true;
}

void BenchmarkRunner::set_stage(const std::string & stage, const std::string & message, int32_t pct) {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.stage        = stage;
    snap_.message      = message;
    snap_.progress_pct = std::clamp(pct, 0, 100);
}

void BenchmarkRunner::push_log(const std::string & line) {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.log.push_back(line);
    if (snap_.log.size() > 200) snap_.log.erase(snap_.log.begin());
}

void BenchmarkRunner::publish(const std::vector<BenchCandidate> & cands, int32_t current) {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.candidates      = cands;
    snap_.candidates_total = (int32_t) cands.size();
    snap_.current_index   = current;
    if (current >= 0 && current < (int32_t) cands.size()) {
        snap_.current_label = cands[(size_t) current].label;
    }
    int32_t done = 0;
    for (const auto & c : cands) if (c.measured || c.ok) ++done;
    snap_.candidates_done = done;
}

// ---------------------------------------------------------------------------
// Plan: derive the best-known-safe configuration without touching the model.
// ---------------------------------------------------------------------------
BenchRecommendation BenchmarkRunner::plan_config(const BenchOptions & opt, std::string & err) const {
    BenchRecommendation rec;

    std::string model_path = opt.model_path;
    if (model_path.empty()) model_path = engine_.current_config().model_path;
    if (model_path.empty()) {
        err = "Не выбрана модель: сначала загрузите .gguf, чтобы рассчитать параметры";
        return rec;
    }

    GgufLayout lay = inspect_gguf_layout(model_path);
    if (!lay.summary.ok) {
        err = "Не удалось прочитать GGUF-заголовок: " + lay.summary.error;
        return rec;
    }
    const ModelSummary & m = lay.summary;
    const SystemMemoryInfo mem = engine_.query_system_memory();

    EngineConfig cfg = engine_.current_config();
    cfg.model_path = model_path;

    const int32_t hw = (int32_t) std::max(1u, std::thread::hardware_concurrency());
    // Ryzen 7 5700X: 8 physical cores / 16 SMT threads -> decode threads = 8,
    // prefill = all 14-16 threads. Half of the SMT count is the safe rule; when
    // the OS only reports physical cores (no SMT) we keep all of them.
    const int32_t phys_threads = resolve_phys_threads(hw);
    cfg.n_threads         = phys_threads;
    cfg.n_threads_batch   = std::max<int32_t>(cfg.n_threads, std::min<int32_t>(hw, (int32_t) (hw * 0.9)));
    rec.reasons.push_back(fmt("Потоки декодирования = %d (физические ядра), prefill = %d (SMT)",
                              cfg.n_threads, cfg.n_threads_batch));

    // Context: keep the user's request but never exceed the trained window.
    if (m.n_ctx_train > 0 && (cfg.n_ctx == 0 || (int32_t) cfg.n_ctx > m.n_ctx_train)) {
        cfg.n_ctx = (uint32_t) m.n_ctx_train;
        rec.reasons.push_back(fmt("Контекст ограничен обученным окном модели: %u токенов", cfg.n_ctx));
    }

    // KV precision: V in f16 keeps Flash-Attention valid on Polaris (head_dim 512).
    cfg.kv_type_k = "q8_0";
    cfg.kv_type_v = "f16";
    rec.reasons.push_back("KV: K=q8_0 (экономия VRAM), V=f16 (Flash Attention на RX 580, head_dim 512)");

    const int32_t ngl = cfg.n_gpu_layers > 0 ? std::min<int32_t>(cfg.n_gpu_layers, std::max(1, m.n_layer)) : 0;
    cfg.n_gpu_layers = ngl;

    const uint64_t kv_mb = estimate_kv_mb(m, cfg, ngl);
    rec.predicted_kv_mb = (double) kv_mb;

    // Hot expert bank: whatever VRAM is left after KV + compute headroom.
    const uint64_t vram_free = mem.vram_free_mb > 0 ? mem.vram_free_mb : 4600;
    uint64_t bank_mb = 0;
    if (m.is_moe && ngl > 0) {
        if (vram_free > kv_mb + kComputeHeadroomMb) {
            bank_mb = vram_free - kv_mb - kComputeHeadroomMb;
        }
        // Never claim more than the model's routed experts actually weigh.
        const uint64_t expert_mb = m.expert_bytes >> 20;
        if (expert_mb > 0) bank_mb = std::min<uint64_t>(bank_mb, expert_mb);
        // Round down to whole expert slots for a clean ClassLRU bank.
        if (m.cycle_floor_mb > 0) bank_mb = (bank_mb / m.cycle_floor_mb) * m.cycle_floor_mb;
        if (bank_mb < 256) bank_mb = 0;
    }
    cfg.vram_cache_mb = (size_t) bank_mb;
    rec.predicted_vram_mb = bank_mb + kv_mb + kComputeHeadroomMb;
    rec.reasons.push_back(bank_mb > 0
        ? fmt("Горячий банк экспертов в VRAM = %llu МБ (свободно %llu МБ − KV %.0f МБ − резерв %llu МБ)",
              (unsigned long long) bank_mb, (unsigned long long) vram_free, (double) kv_mb,
              (unsigned long long) kComputeHeadroomMb)
        : std::string("VRAM недостаточно под банк экспертов: эксперты пойдут через DDR4 + CPU (KV остаётся на GPU)"));

    // DDR4 arena: keep commit headroom so Windows never swaps to the HDD.
    const uint64_t mem_avail = mem.commit_avail_mb > 0 ? mem.commit_avail_mb
                            : (mem.ram_avail_mb > 0 ? mem.ram_avail_mb : 9728);
    uint64_t ram_mb = (uint64_t) std::max<double>(0.0, (double) mem_avail * 0.80 - 1024.0);
    // Cap by what actually has to live in RAM: experts + dense weights.
    const uint64_t need_ram_mb = (m.expert_bytes + m.dense_bytes) >> 20;
    if (need_ram_mb > 0) ram_mb = std::min<uint64_t>(ram_mb, need_ram_mb);
    ram_mb = (ram_mb / 128) * 128;
    cfg.ram_cache_mb = (size_t) ram_mb;
    rec.predicted_ram_mb = ram_mb;
    rec.reasons.push_back(fmt("Арена DDR4 = %llu МБ (80%% свободного commit − 1 ГБ резерва)", (unsigned long long) ram_mb));

    // Microbatch: the MoE cache only handles 32 rows, more is wasted work.
    cfg.n_ubatch  = (m.is_moe || cfg.moe_stream) ? 32u : 64u;
    cfg.n_batch   = std::max<uint32_t>(cfg.n_batch, cfg.n_ubatch * 8);
    rec.reasons.push_back(fmt("Микробатч = %u (жёсткий лимит кэша экспертов llama_moe_cache)", cfg.n_ubatch));

    // Hybrid split: with a big VRAM bank the GPU already carries the hot path,
    // so misses go mostly to Ryzen in parallel (0.85); otherwise lean on CPU.
    cfg.cpu_share = (bank_mb >= 2048) ? 0.85f : 0.90f;
    cfg.auto_balance_compute = true;
    cfg.zero_hdd_mode        = true;
    cfg.moe_stream           = true;
    rec.reasons.push_back(fmt("Начальная доля CPU ∥ GPU = %.2f (авто-балансировщик дальше подстроит сам)",
                              cfg.cpu_share));

    BenchCandidate probe;
    probe.cfg             = cfg;
    probe.vram_used_mb    = cfg.vram_cache_mb;
    probe.ram_used_mb     = cfg.ram_cache_mb;
    rec.predicted_decode_tps = projected_decode_tps(probe, phys_threads);
    rec.cfg = cfg;
    rec.ok  = true;
    return rec;
}

// ---------------------------------------------------------------------------
// Full: real coordinate descent with model reloads.
// ---------------------------------------------------------------------------
bool BenchmarkRunner::measure_candidate(BenchCandidate & cand, const BenchOptions & opt,
                                        const std::string & prompt, std::string & err) {
    const int64_t t0 = now_ms();

    EngineConfig cfg = cand.cfg;
    cfg.model_path = opt.model_path.empty() ? engine_.current_config().model_path : opt.model_path;

    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.message = "Загрузка модели: " + cand.label;
    }
    engine_.unload_model();
    if (cancel_.load(std::memory_order_acquire)) {
        err = "Остановлено пользователем";
        return false;
    }
    if (!engine_.load_model(cfg, err)) {
        return false;
    }

    const auto before = engine_.telemetry();

    // Wall-clock timing: the engine's live counters are smoothed/atomic and can
    // report absurd values for very short runs, so measure first-token latency
    // and steady-state decode directly from the token callback.
    std::string reply;
    const int32_t n_tok = std::clamp<int32_t>(opt.tokens, 16, 512);
    const auto t_gen0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point t_first{};
    int64_t pieces = 0;
    const bool gen_ok = engine_.generate_raw(
        prompt, n_tok, /*temperature=*/0.0f, opt.seed,
        [&](const std::string & piece) -> bool {
            if (pieces == 0) t_first = std::chrono::steady_clock::now();
            ++pieces;
            (void) piece;
            return true;
        },
        reply, err);
    if (!gen_ok) return false;
    const auto t_gen1 = std::chrono::steady_clock::now();

    const double total_s = std::chrono::duration<double>(t_gen1 - t_gen0).count();
    const double ttft_s  = pieces > 0
        ? std::chrono::duration<double>(t_first - t_gen0).count() : total_s;
    const double decode_s = std::max(1e-6, total_s - ttft_s);
    const double n_pieces = (double) std::max<int64_t>(1, pieces);
    const auto after = engine_.telemetry();

    cand.measured      = true;
    cand.ok            = true;
    cand.decode_tps    = opt.tokens > 0 ? (n_pieces - 1.0) / decode_s : n_pieces / total_s;
    if (!(cand.decode_tps > 0.0) || !std::isfinite(cand.decode_tps)) cand.decode_tps = n_pieces / std::max(1e-6, total_s);
    cand.prefill_tps   = after.prefill_tps;
    cand.ttft_ms       = ttft_s * 1000.0;
    if (cand.prefill_tps <= 0.0) {
        const uint64_t prompt_before = 0;  // counters are cumulative; use total for the estimate
        (void) prompt_before;
        cand.prefill_tps = after.prompt_tokens > 0
            ? (double) after.prompt_tokens / std::max(1e-6, ttft_s) : 0.0;
    }
    cand.hdd_reads     = after.ssd_expert_reads - before.ssd_expert_reads;
    cand.ram_used_mb   = after.ram_arena_used_mb;
    cand.vram_used_mb  = after.vram_prewarmed_mb > 0 ? after.vram_prewarmed_mb : after.vram_cache_budget_mb;
    const uint64_t served = after.gpu_expert_hits + after.cpu_parallel_experts +
                            (after.ram_cache_hits - before.ram_cache_hits);
    if (served > 0) {
        cand.vram_hit_pct = 100.0 * (double) after.gpu_expert_hits / (double) served;
    } else {
        cand.vram_hit_pct = 100.0;
    }
    // Score: throughput first, but punish disk traffic and starved VRAM banks so
    // "fast but re-reading the HDD every token" never wins.
    const double hdd_penalty = 1.0 + std::min<double>(1.5, (double) cand.hdd_reads * 0.02);
    const double hit_bonus   = 0.85 + 0.15 * (cand.vram_hit_pct / 100.0);
    cand.score    = (cand.decode_tps * hit_bonus) / hdd_penalty;
    cand.elapsed_s = (double) (now_ms() - t0) / 1000.0;
    return true;
}

bool BenchmarkRunner::start(const BenchOptions & opt, std::string & err) {
    if (running_.load(std::memory_order_acquire)) {
        err = "Бенчмарк уже выполняется";
        return false;
    }
    if (engine_.is_loading()) {
        err = "Дождитесь окончания загрузки модели";
        return false;
    }
    std::string model_path = opt.model_path.empty() ? engine_.current_config().model_path : opt.model_path;
    if (model_path.empty()) {
        err = "Не выбрана модель (.gguf)";
        return false;
    }
    if (opt.mode == BenchMode::Full && !engine_.is_loaded()) {
        // Full mode is allowed from cold start: the first candidate loads it.
    }

    cancel_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = BenchmarkSnapshot{};
        snap_.stage      = "planning";
        snap_.mode       = opt.mode == BenchMode::Full ? "full" : "plan";
        snap_.model_path = model_path;
        snap_.message    = "Анализ модели и свободной памяти...";
        snap_.log.push_back("Запуск бенчмарка: " + model_path);
    }

    if (worker_.joinable()) worker_.join();
    BenchOptions o = opt;
    o.model_path   = model_path;
    if (o.prompt.empty()) o.prompt = kDefaultPrompt;
    worker_ = std::thread([this, o]() {
        if (o.mode == BenchMode::Full) run_full(o);
        else                           run_plan(o);
        running_.store(false, std::memory_order_release);
    });
    return true;
}

void BenchmarkRunner::cancel() {
    cancel_.store(true, std::memory_order_release);
    if (engine_.is_loaded() || engine_.is_loading()) {
        // Interrupt the greedy generation and any in-flight load.
        engine_.request_cancel();
    }
}

void BenchmarkRunner::run_plan(const BenchOptions & opt) {
    std::string err;
    BenchRecommendation rec = plan_config(opt, err);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!rec.ok) {
            snap_.stage = "failed";
            snap_.error = err;
            snap_.message = err;
            return;
        }
        snap_.recommendation = rec;
        snap_.stage    = "done";
        snap_.message  = "Параметры рассчитаны (без перезагрузки модели)";
        snap_.progress_pct = 100;
        snap_.model_name = opt.model_path;
        for (const auto & r : rec.reasons) snap_.log.push_back(r);
    }
}

void BenchmarkRunner::run_full(const BenchOptions & opt) {
    std::string err;

    // 1. Plan first: the best-known-safe config is always candidate #0.
    BenchRecommendation rec = plan_config(opt, err);
    if (!rec.ok) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.stage   = "failed";
        snap_.error   = err;
        snap_.message = err;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.recommendation = rec;
        for (const auto & r : rec.reasons) snap_.log.push_back("План: " + r);
    }

    const SystemMemoryInfo mem = engine_.query_system_memory();
    const int32_t hw = (int32_t) std::max(1u, std::thread::hardware_concurrency());
    const int32_t phys = resolve_phys_threads(hw);

    // 2. Build the candidate grid: one variable changed at a time around the plan.
    std::vector<BenchCandidate> cands;
    auto signature = [](const EngineConfig & c) {
        // Every candidate is one full model reload, so identical knob sets must
        // never be measured twice (the plan config often equals a sweep point).
        return std::to_string(c.n_threads) + "/" + fmt("%.3f", c.cpu_share) + "/" +
               std::to_string(c.vram_cache_mb) + "/" + std::to_string(c.n_ubatch);
    };
    std::vector<std::string> seen;
    auto add = [&](const EngineConfig & c, const std::string & label, const std::vector<std::string> & knobs) {
        if ((int32_t) cands.size() >= std::max<int32_t>(1, opt.max_candidates)) return;
        const std::string sig = signature(c);
        if (std::find(seen.begin(), seen.end(), sig) != seen.end()) return;
        seen.push_back(sig);
        BenchCandidate b;
        b.index    = (int32_t) cands.size();
        b.label    = label;
        b.knobs    = knobs;
        b.cfg      = c;
        b.cfg.model_path = opt.model_path;
        cands.push_back(std::move(b));
    };

    add(rec.cfg, "План движка", {"потоки=" + std::to_string(rec.cfg.n_threads),
                                 "cpu_share=" + fmt("%.2f", rec.cfg.cpu_share),
                                 "vram=" + std::to_string(rec.cfg.vram_cache_mb / 128 * 128) + "МБ"});

    if (opt.sweep_threads) {
        for (int32_t t : {phys, std::min(hw, phys * 2), std::max(1, phys / 2)}) {
            EngineConfig c = rec.cfg;
            c.n_threads = t;
            add(c, "потоки " + std::to_string(t), {"потоки=" + std::to_string(t)});
        }
    }
    if (opt.sweep_cpu_share) {
        for (float s : {0.96f, 0.82f, 0.65f}) {
            EngineConfig c = rec.cfg;
            c.cpu_share = s;
            add(c, "CPU ∥ GPU " + fmt("%.2f", s), {"cpu_share=" + fmt("%.2f", s)});
        }
    }
    if (opt.sweep_vram_bank && rec.cfg.vram_cache_mb > 512) {
        for (double f : {0.65, 1.0}) {
            EngineConfig c = rec.cfg;
            c.vram_cache_mb = (size_t) std::max<uint64_t>(256, (uint64_t) ((double) rec.cfg.vram_cache_mb * f));
            if (c.vram_cache_mb == rec.cfg.vram_cache_mb) continue;
            add(c, fmt("банк VRAM %llu МБ", (unsigned long long) c.vram_cache_mb),
                {fmt("vram=%.0f%%", f * 100.0)});
        }
    }
    if (opt.sweep_ubatch) {
        for (uint32_t ub : {16u, 32u}) {
            if (ub == rec.cfg.n_ubatch) continue;
            EngineConfig c = rec.cfg;
            c.n_ubatch = ub;
            add(c, "микробатч " + std::to_string(ub), {"ubatch=" + std::to_string(ub)});
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.candidates       = cands;
        snap_.candidates_total = (int32_t) cands.size();
        snap_.model_name       = rec.cfg.model_path;
    }
    push_log(fmt("Кандидатов к измерению: %zu (каждый = перезагрузка модели)", cands.size()));

    const int64_t t_start = now_ms();
    bool canceled = false;

    for (size_t i = 0; i < cands.size(); ++i) {
        if (cancel_.load(std::memory_order_acquire)) { canceled = true; break; }
        BenchCandidate & c = cands[i];
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.current_index = (int32_t) i;
            snap_.current_label = c.label;
            snap_.progress_pct  = (int32_t) ((double) i * 100.0 / (double) cands.size());
            snap_.message       = "Измерение " + c.label;
            snap_.candidates    = cands;
        }
        push_log(fmt("[%zu/%zu] %s", i + 1, cands.size(), c.label.c_str()));

        std::string cerr;
        if (!measure_candidate(c, opt, opt.prompt, cerr)) {
            c.ok    = false;
            c.error = cerr;
            push_log("  ошибка: " + cerr);
            if (cerr == "Остановлено пользователем") { canceled = true; break; }
        } else {
            push_log(fmt("  %.2f tok/s • prefill %.1f tok/s • VRAM-попадания %.0f%% • HDD %llu чтений",
                         c.decode_tps, c.prefill_tps, c.vram_hit_pct, (unsigned long long) c.hdd_reads));
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.candidates = cands;
            snap_.candidates_done = (int32_t) i + 1;
            snap_.elapsed_s  = (double) (now_ms() - t_start) / 1000.0;
        }
    }

    // 3. Pick the winner (highest score among successful measurements).
    int32_t best = -1;
    double best_score = -1.0;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (cands[i].ok && cands[i].measured && cands[i].score > best_score) {
            best_score = cands[i].score;
            best = (int32_t) i;
        }
    }
    if (best >= 0) cands[(size_t) best].best = true;

    // 4. Leave the winner loaded so the chat continues with tuned parameters.
    if (best >= 0 && !canceled) {
        // The last measured candidate is already loaded in the winner's slot only
        // by chance, so reload deterministically (one extra reload, not per token).
        BenchCandidate & w = cands[(size_t) best];
        std::string lerr;
        push_log("Победитель: " + w.label + " — оставляю модель загруженной с этими параметрами");
        engine_.unload_model();
        if (!engine_.load_model(w.cfg, lerr)) {
            push_log("Не удалось перезагрузить победителя: " + lerr);
        }
    }

    std::lock_guard<std::mutex> lk(mu_);
    snap_.candidates       = cands;
    snap_.candidates_done  = (int32_t) std::count_if(cands.begin(), cands.end(),
                                                     [](const BenchCandidate & c) { return c.ok && c.measured; });
    snap_.elapsed_s        = (double) (now_ms() - t_start) / 1000.0;
    snap_.best_index       = best;
    snap_.progress_pct     = 100;
    snap_.stage            = canceled ? "canceled" : (best >= 0 ? "done" : "failed");
    if (best >= 0) {
        snap_.recommendation.cfg                = cands[(size_t) best].cfg;
        snap_.recommendation.ok                 = true;
        snap_.recommendation.predicted_decode_tps = cands[(size_t) best].decode_tps;
        snap_.message = fmt("Лучший результат: %s — %.2f tok/s", cands[(size_t) best].label.c_str(),
                            cands[(size_t) best].decode_tps);
        snap_.recommendation.reasons.push_back(snap_.message);
    } else if (canceled) {
        snap_.message = "Бенчмарк остановлен пользователем";
    } else {
        snap_.message = "Ни один кандидат не завершился успешно: " + snap_.error;
    }
}

} // namespace dualmoe
