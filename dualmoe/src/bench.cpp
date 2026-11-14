#include "bench.h"

#include "gguf_inspect.h"
#include "autobudget.h"

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

    // Existing experts -> VRAM bank (first), then DDR4 (second), then disk. The plan uses the very same
    // AutoBudget the loader uses, so "what the benchmark promises" == "what the load will do".
    const uint64_t dense_gpu_mb = (ngl > 0 && m.n_layer > 0)
        ? (m.dense_bytes >> 20) * (uint64_t) std::min<int32_t>(ngl, m.n_layer) / (uint64_t) m.n_layer : 0;
    const AutoBudgetPlan budget = plan_auto_budget(m, mem, cfg, ngl, kv_mb, dense_gpu_mb);
    uint64_t bank_mb = 0;
    uint64_t ram_mb  = 0;
    if (budget.ok) {
        bank_mb = std::max<uint64_t>(budget.vram_bank_mb, budget.vram_bank_mb ? 1 : 0);
        ram_mb  = budget.ram_arena_mb;
        for (const std::string & r : budget.reasons) rec.reasons.push_back("Бюджет: " + r);
    }
    cfg.vram_cache_mb = (size_t) bank_mb;
    rec.predicted_vram_mb = bank_mb + kv_mb + kComputeHeadroomMb;
    cfg.ram_cache_mb = (size_t) ram_mb;
    rec.predicted_ram_mb = ram_mb;

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

    // Cache classes: reserve part of the hot bank for the experts that every request re-uses, and
    // keep the warm expert set between runs. The RAM side always keeps the full hot set, so the
    // reservation grows with the bank: 30% at 1.5 GB, 40% from 3 GB (the VRAM is what is scarce).
    cfg.static_bank_share = bank_mb >= 3072 ? 0.40f : (bank_mb >= 1536 ? 0.30f : 0.25f);
    cfg.static_bank_mb    = 0;
    cfg.warm_state        = true;
    if (bank_mb > 0) {
        rec.reasons.push_back(fmt("Класс «статических» экспертов (структура + системный промпт) = %.0f%% банка ≈ %llu МБ: смена темы не выбивает их из VRAM",
                                  (double) (cfg.static_bank_share * 100.0f),
                                  (unsigned long long) ((uint64_t) (bank_mb * (uint64_t) (cfg.static_bank_share * 100)) / 100)));
    } else {
        rec.reasons.push_back("VRAM-банка нет: горячие эксперты закрепляются в арене DDR4, набор прогрева сохраняется между запусками");
    }

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

    // Warm-up: the first tokens after a load also fill the expert caches, which would unfairly
    // favour whichever candidate happened to be measured first. Discard a few tokens first.
    {
        const int32_t warm = std::clamp<int32_t>(opt.warmup_tokens, 0, 64);
        if (warm > 0) {
            std::string sink;
            engine_.generate_raw(prompt, warm, 0.0f, opt.seed, nullptr, sink, err);
        }
    }

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

// ---------------------------------------------------------------------------
// Measurement on the context that is already loaded (no reload).
// Threads, CPU ∥ GPU share and the class split are applied through apply_live_config, so a sweep
// over them costs one warm-up + one generation per point instead of a full model load.
// ---------------------------------------------------------------------------
bool BenchmarkRunner::apply_live_knobs(const EngineConfig & cfg) {
    std::string err;
    return engine_.apply_live_config(cfg, err);
}

bool BenchmarkRunner::measure_loaded(BenchCandidate & cand, const BenchOptions & opt,
                                     const std::string & prompt, std::string & err) {
    const int64_t t0 = now_ms();
    const EngineConfig cur = engine_.current_config();
    EngineConfig probe = cand.cfg;
    // keep the live context consistent with what is actually measurable in place
    probe.n_ctx          = cur.n_ctx;
    probe.kv_type_k      = cur.kv_type_k;
    probe.kv_type_v      = cur.kv_type_v;
    probe.vram_cache_mb  = cur.vram_cache_mb;

    if (!apply_live_knobs(probe)) {
        return false;
    }
    const auto before = engine_.telemetry();

    std::string reply;
    std::string sink;
    const int32_t warm = std::clamp<int32_t>(opt.warmup_tokens, 0, 64);
    if (warm > 0) {
        engine_.generate_raw(prompt, warm, 0.0f, opt.seed, nullptr, sink, err);
    }

    const int32_t n_tok = std::clamp<int32_t>(opt.tokens, 16, 512);
    const auto t_gen0 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point t_first{};
    int64_t pieces = 0;
    const bool gen_ok = engine_.generate_raw(
        prompt, n_tok, 0.0f, opt.seed,
        [&](const std::string &) -> bool {
            if (pieces == 0) t_first = std::chrono::steady_clock::now();
            ++pieces;
            return true;
        },
        reply, err);
    if (!gen_ok) return false;
    const auto t_gen1 = std::chrono::steady_clock::now();
    const auto after = engine_.telemetry();

    const double total_s  = std::chrono::duration<double>(t_gen1 - t_gen0).count();
    const double ttft_s   = pieces > 0 ? std::chrono::duration<double>(t_first - t_gen0).count() : total_s;
    const double decode_s = std::max(1e-6, total_s - ttft_s);
    const double n_pieces = (double) std::max<int64_t>(1, pieces);

    cand.measured     = true;
    cand.in_place     = true;
    cand.ok           = true;
    cand.cfg.n_threads = probe.n_threads;
    cand.cfg.cpu_share = probe.cpu_share;
    cand.decode_tps   = (n_pieces - 1.0) / decode_s;
    if (!(cand.decode_tps > 0.0) || !std::isfinite(cand.decode_tps)) cand.decode_tps = n_pieces / std::max(1e-6, total_s);
    cand.ttft_ms      = ttft_s * 1000.0;
    cand.prefill_tps  = after.prefill_tps;
    if (cand.prefill_tps <= 0.0 && after.prompt_tokens > 0) {
        cand.prefill_tps = (double) after.prompt_tokens / std::max(1e-6, ttft_s);
    }
    cand.hdd_reads    = after.ssd_expert_reads - before.ssd_expert_reads;
    cand.ram_used_mb  = after.ram_arena_used_mb;
    cand.vram_used_mb = after.vram_prewarmed_mb > 0 ? after.vram_prewarmed_mb : after.vram_cache_budget_mb;
    const uint64_t served = after.gpu_expert_hits + after.cpu_parallel_experts +
                            (after.ram_cache_hits - before.ram_cache_hits);
    cand.vram_hit_pct = served > 0 ? 100.0 * (double) after.gpu_expert_hits / (double) served : 100.0;
    const double hdd_penalty = 1.0 + std::min<double>(1.5, (double) cand.hdd_reads * 0.02);
    const double hit_bonus   = 0.85 + 0.15 * (cand.vram_hit_pct / 100.0);
    cand.score       = (cand.decode_tps * hit_bonus) / hdd_penalty;
    cand.elapsed_s   = (double) (now_ms() - t0) / 1000.0;
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

    // 1. Plan first: the best-known-safe config is always the starting point.
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
    const int32_t cap  = std::max<int32_t>(1, opt.max_candidates);

    std::vector<BenchCandidate> cands;
    auto signature = [](const EngineConfig & c) {
        // A full reload is expensive, so two candidates may only share it when every knob that needs
        // the reload matches (threads / share are applied live and can differ inside one group).
        return std::to_string(c.vram_cache_mb) + "/" + std::to_string(c.n_ubatch);
    };
    auto add = [&](const EngineConfig & c, const std::string & label, const std::vector<std::string> & knobs) {
        if ((int32_t) cands.size() >= cap) return false;
        BenchCandidate b;
        b.index = (int32_t) cands.size();
        b.label = label;
        b.knobs = knobs;
        b.cfg   = c;
        b.cfg.model_path = opt.model_path;
        cands.push_back(std::move(b));
        return true;
    };

    // Group 0: the plan itself. Its reload is reused by every "cheap" (in-place) probe below.
    const std::string plan_sig = signature(rec.cfg);
    add(rec.cfg, "План движка", {"потоки=" + std::to_string(rec.cfg.n_threads),
                                 "cpu_share=" + fmt("%.2f", rec.cfg.cpu_share),
                                 "vram=" + std::to_string(rec.cfg.vram_cache_mb / 128 * 128) + "МБ"});

    // The cheap axes around the plan, measured on the already loaded context. Stage 1 tests the plan
    // value plus the two extremes, then stage 2 only fills the gap where the winner sits.
    struct Sweep { bool enabled; EngineConfig (*mk)(const EngineConfig &, double); std::vector<double> stage1; std::vector<double> all; std::string label; std::string knob; };
    std::vector<Sweep> sweeps;
    const std::vector<double> thr_pts = { (double) std::max(1, phys / 2), (double) phys, (double) std::min(hw, phys * 2) };
    if (opt.sweep_threads) {
        sweeps.push_back({true, [](const EngineConfig & base, double v) {
            EngineConfig c = base; c.n_threads = (int32_t) v; return c; },
            {thr_pts[1], thr_pts[0], thr_pts[2]},
            {thr_pts[0], (double) std::max(1, (int32_t) (thr_pts[0] + thr_pts[1]) / 2), thr_pts[1], thr_pts[2]},
            "потоки", "потоки="});
    }
    if (opt.sweep_cpu_share) {
        const double plan_share = (double) rec.cfg.cpu_share;
        sweeps.push_back({true, [](const EngineConfig & base, double v) {
            EngineConfig c = base; c.cpu_share = (float) v; return c; },
            {plan_share, 0.65, 0.96},
            {0.60, 0.70, 0.80, 0.90, 0.96},
            "CPU ∥ GPU", "cpu_share="});
    }
    // Forced seam: the only axis that makes the CPU compute experts of the SAME token the GPU is working
    // on. 0 (off) is always in the set, so the sweep can prove that the seam is not slower.
    if (opt.sweep_seam) {
        sweeps.push_back({true, [](const EngineConfig & base, double v) {
            EngineConfig c = base; c.parallel_seam_share = (float) v; return c; },
            {0.0, 0.25, 0.45},
            {0.0, 0.15, 0.30, 0.45},
            "GPU ∥ CPU", "seam="});
    }

    // Group 1+: the VRAM bank sizes, each of which is one reload.
    std::vector<EngineConfig> groups;
    groups.push_back(rec.cfg);
    if (opt.sweep_vram_bank && rec.cfg.vram_cache_mb > 512) {
        for (double f : {0.65, 1.0}) {
            EngineConfig c = rec.cfg;
            c.vram_cache_mb = (size_t) std::max<uint64_t>(256, (uint64_t) ((double) rec.cfg.vram_cache_mb * f));
            if (c.vram_cache_mb == rec.cfg.vram_cache_mb) continue;
            bool dup = false;
            for (const auto & g : groups) if (g.vram_cache_mb == c.vram_cache_mb) dup = true;
            if (!dup) groups.push_back(c);
        }
    }
    if (opt.sweep_ubatch && rec.cfg.n_ubatch != 16u) {
        EngineConfig c = rec.cfg;
        c.n_ubatch = 16u;
        groups.push_back(c);
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.candidates       = cands;
        snap_.candidates_total = cap;
        snap_.model_name       = rec.cfg.model_path;
    }
    push_log(fmt("План готов. Быстрый режим: %s. Перезагрузок модели (банк VRAM / микробатч): до %zu",
                 opt.fast ? "дешёвые параметры меряются без перезагрузки" : "полный перебор", groups.size()));

    const int64_t t_start = now_ms();
    auto publish_all = [&](int32_t current) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.candidates = cands;
        snap_.candidates_total = (int32_t) cands.size();
        snap_.current_index = current;
        if (current >= 0 && current < (int32_t) cands.size()) snap_.current_label = cands[(size_t) current].label;
        snap_.candidates_done = (int32_t) std::count_if(cands.begin(), cands.end(),
            [](const BenchCandidate & c) { return c.ok && c.measured; });
        snap_.elapsed_s = (double) (now_ms() - t_start) / 1000.0;
    };
    auto measure_at = [&](size_t i, bool allow_in_place = false) -> bool {
        BenchCandidate & c = cands[i];
        {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.current_index = (int32_t) i;
            snap_.current_label = c.label;
            snap_.message       = "Измерение " + c.label;
        }
        std::string cerr;
        bool ok = false;
        // Cheap knobs (threads / CPU ∥ GPU share) are applied to the live context: no model reload.
        if (allow_in_place && opt.fast && engine_.is_loaded()) {
            ok = measure_loaded(c, opt, opt.prompt, cerr);
            if (!ok) {
                cerr.clear();      // fall back to a full reload when the context refused the change
                c.in_place = false;
                ok = measure_candidate(c, opt, opt.prompt, cerr);
            }
        } else {
            ok = measure_candidate(c, opt, opt.prompt, cerr);
        }
        if (!ok) {
            c.ok = false;
            c.error = cerr;
            push_log("  ошибка: " + cerr);
        } else {
            push_log(fmt("  %s — %.2f tok/s • TTFT %.0f мс • VRAM-попадания %.0f%% • HDD %llu",
                         c.in_place ? "на месте" : "после перезагрузки", c.decode_tps, c.ttft_ms,
                         c.vram_hit_pct, (unsigned long long) c.hdd_reads));
        }
        publish_all((int32_t) i);
        return ok;
    };

    bool canceled = false;

    // 2. Walk the reload groups; the cheap axes are tuned inside each group right after its load.
    for (size_t g = 0; g < groups.size() && !canceled; ++g) {
        if (cancel_.load(std::memory_order_acquire)) { canceled = true; break; }
        if (g > 0 && (int32_t) cands.size() >= cap) {
            push_log("Достигнут лимит кандидатов — оставшиеся точки банка VRAM пропущены");
            break;
        }

        if (g == 0) {
            if (!measure_at(0)) {
                if (cands[0].error == "Остановлено пользователем") { canceled = true; break; }
                push_log("План не загрузился — пробую остальные точки банка VRAM");
                continue;
            }
        } else {
            const EngineConfig & gc = groups[g];
            std::string label = (gc.vram_cache_mb != rec.cfg.vram_cache_mb)
                ? fmt("банк VRAM %llu МБ", (unsigned long long) gc.vram_cache_mb)
                : ("микробатч " + std::to_string(gc.n_ubatch) + " (банк VRAM " +
                   std::to_string(gc.vram_cache_mb / 128 * 128) + " МБ)");
            if (!add(gc, label, {(gc.vram_cache_mb != rec.cfg.vram_cache_mb)
                                    ? fmt("vram=%.0f%%", 100.0 * (double) gc.vram_cache_mb / (double) std::max<size_t>(1, rec.cfg.vram_cache_mb))
                                    : ("ubatch=" + std::to_string(gc.n_ubatch))})) {
                break;
            }
            const size_t gi = cands.size() - 1;
            if (!measure_at(gi)) {
                if (cands[gi].error == "Остановлено пользователем") { canceled = true; break; }
                continue;
            }
        }

        // --- cheap axes on the loaded context -------------------------------------------------
        if (!opt.fast) continue;
        for (const Sweep & sw : sweeps) {
            if (cancel_.load(std::memory_order_acquire)) { canceled = true; break; }
            std::vector<double> pts = sw.stage1;
            std::vector<std::pair<double, double>> scored; // (tps, value)
            for (double v : pts) {
                if ((int32_t) cands.size() >= cap) break;
                EngineConfig c = sw.mk(groups[g], v);
                const bool pct = sw.knob == "cpu_share=" || sw.knob == "seam=";
                const std::string knob_val = pct ? fmt("%.2f", v) : std::to_string((int) v);
                const std::string lbl = sw.label + " " + knob_val;
                if (!add(c, lbl, {sw.knob + knob_val})) break;
                const size_t i = cands.size() - 1;
                if (measure_at(i, /*allow_in_place=*/true) && cands[i].ok) {
                    scored.push_back({cands[i].decode_tps, v});
                }
            }
            if (scored.empty()) continue;
            const auto best = *std::max_element(scored.begin(), scored.end(),
                                                [](const auto & a, const auto & b) { return a.first < b.first; });
            // Stage 2: refine only in the interval around the winner (2 extra points at most).
            for (double v : sw.all) {
                if ((int32_t) cands.size() >= cap) break;
                const bool near = v > best.second - 0.08 || (sw.knob == "потоки=" && v > best.second - 4);
                if (!near) continue;
                if (std::abs(v - best.second) < 1e-9) continue;
                bool dup = false;
                for (const auto & s2 : scored) if (std::abs(s2.second - v) < 1e-9) dup = true;
                if (dup) continue;
                EngineConfig c = sw.mk(groups[g], v);
                const std::string lbl = sw.label + " " + (sw.knob == "cpu_share=" ? fmt("%.2f", v) : std::to_string((int) v));
                if (!add(c, lbl, {sw.knob + (sw.knob == "cpu_share=" ? fmt("%.2f", v) : std::to_string((int) v))})) break;
                measure_at(cands.size() - 1, /*allow_in_place=*/true);
                if ((int32_t) cands.size() >= cap) break;
            }
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

    // 4. Leave the winner active: cheap knobs apply in place, only a different bank needs a reload.
    if (best >= 0 && !canceled) {
        BenchCandidate & w = cands[(size_t) best];
        const EngineConfig cur = engine_.current_config();
        std::string lerr;
        push_log("Победитель: " + w.label + " — применяю параметры");
        const bool same_bank = w.cfg.vram_cache_mb == cur.vram_cache_mb && w.cfg.n_ubatch == cur.n_ubatch &&
                               w.cfg.n_ctx == cur.n_ctx;
        if (same_bank && apply_live_knobs(w.cfg)) {
            push_log("Параметры применены без перезагрузки модели");
        } else {
            engine_.unload_model();
            if (!engine_.load_model(w.cfg, lerr)) {
                push_log("Не удалось перезагрузить победителя: " + lerr);
            }
        }
    }

    std::lock_guard<std::mutex> lk(mu_);
    snap_.candidates       = cands;
    snap_.candidates_total = (int32_t) cands.size();
    snap_.candidates_done  = (int32_t) std::count_if(cands.begin(), cands.end(),
                                                     [](const BenchCandidate & c) { return c.ok && c.measured; });
    snap_.elapsed_s        = (double) (now_ms() - t_start) / 1000.0;
    snap_.best_index       = best;
    snap_.progress_pct     = 100;
    snap_.stage            = canceled ? "canceled" : (best >= 0 ? "done" : "failed");
    if (best >= 0) {
        snap_.recommendation.cfg                  = cands[(size_t) best].cfg;
        snap_.recommendation.ok                   = true;
        snap_.recommendation.predicted_decode_tps = cands[(size_t) best].decode_tps;
        snap_.message = fmt("Лучший результат: %s — %.2f tok/s (за %.1f с)",
                            cands[(size_t) best].label.c_str(), cands[(size_t) best].decode_tps,
                            snap_.elapsed_s);
        snap_.recommendation.reasons.push_back(snap_.message);
    } else if (canceled) {
        snap_.message = "Бенчмарк остановлен пользователем";
    } else {
        snap_.message = "Ни один кандидат не завершился успешно: " + snap_.error;
    }
}

} // namespace dualmoe
