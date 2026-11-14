#include "autobudget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dualmoe {

namespace {

std::string fmt_mb(uint64_t mb) {
    return std::to_string((unsigned long long) mb) + " МБ";
}

std::string fmt_gb(double mb) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f ГБ", mb / 1024.0);
    return std::string(buf);
}

constexpr uint64_t kMiB = 1ull << 20;

uint64_t ceil_mb(uint64_t bytes) { return (bytes + kMiB - 1) / kMiB; }

// How many whole experts fit into a byte budget, capped by how many are still homeless.
uint64_t experts_fitting(uint64_t budget_bytes, uint64_t per_expert_bytes, uint64_t max_experts) {
    if (per_expert_bytes == 0) return 0;
    return std::min<uint64_t>(budget_bytes / per_expert_bytes, max_experts);
}

} // namespace

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

uint64_t resolve_vram_reserve_mb(uint64_t vram_total_mb, int64_t requested_mb) {
    if (requested_mb >= 0) return (uint64_t) requested_mb;
    // Automatic: the driver needs room for the swap chain, the WebView2/Chromium compositor and the
    // Vulkan staging buffers. 5% + 256 MiB is ~660 MiB on an 8 GB RX 580 — enough for a browser on
    // the second monitor, small enough not to waste a whole expert layer.
    return vram_total_mb * 5 / 100 + 256;
}

uint64_t resolve_ram_reserve_mb(uint64_t ram_total_mb, int64_t requested_mb) {
    if (requested_mb >= 0) return (uint64_t) requested_mb;
    // Automatic: 10% + 1 GB. Windows 11 alone idles at 4-6 GB with a browser, and the model also
    // needs its dense weights + KV + the 32-row compute graph on top of the expert arena.
    return ram_total_mb * 10 / 100 + 1024;
}

AutoBudgetPlan plan_auto_budget(const ModelSummary & model,
                                const SystemMemoryInfo & mem,
                                const EngineConfig & cfg,
                                int32_t ngl,
                                uint64_t kv_mb,
                                uint64_t dense_gpu_mb) {
    AutoBudgetPlan p;
    if (!model.ok) {
        p.error = "Модель не прочитана: нечего распределять";
        return p;
    }

    p.expert_total_mb = model.expert_bytes >> 20;
    p.total_experts   = (uint64_t) std::max<int32_t>(1, model.n_layer) * (uint64_t) std::max<int32_t>(1, model.n_expert);

    // Smallest indivisible step: one expert of one layer. The bank and the arena can only grow by
    // whole experts (the cache stores per-expert slots), so every budget is aligned to this.
    uint64_t per_expert_bytes = 0;
    if (p.expert_total_mb > 0 && model.n_expert > 0 && model.n_layer > 0) {
        per_expert_bytes = model.expert_bytes / ((uint64_t) model.n_expert * (uint64_t) model.n_layer);
    }
    p.per_expert_mb = std::max<uint64_t>(1, ceil_mb(per_expert_bytes));
    const uint64_t max_experts = std::max<uint64_t>(1, p.total_experts);
    const uint64_t expert_total_bytes = model.expert_bytes;

    // ---- 1. VRAM ---------------------------------------------------------------------------------
    p.vram_free_mb    = mem.vram_free_mb;
    p.vram_reserve_mb = resolve_vram_reserve_mb(mem.vram_total_mb > 0 ? mem.vram_total_mb : mem.vram_free_mb,
                                               (int64_t) cfg.vram_reserve_mb);
    p.kv_mb           = kv_mb;
    p.dense_gpu_mb    = dense_gpu_mb;

    if (ngl > 0 && p.vram_free_mb > 0) {
        // KV and dense weights are on the GPU already; whatever is left after them (and after the
        // reserve the user chose) is the expert bank.
        const uint64_t used_mb = p.kv_mb + p.dense_gpu_mb + p.vram_reserve_mb;
        if (p.vram_free_mb > used_mb) {
            const uint64_t budget_b = (p.vram_free_mb - used_mb) * kMiB;
            p.vram_experts = experts_fitting(budget_b, per_expert_bytes, max_experts);
            if (per_expert_bytes > 0) {
                p.vram_bank_mb = ceil_mb(p.vram_experts * per_expert_bytes);
                if (p.vram_bank_mb > p.vram_free_mb - used_mb) p.vram_bank_mb = p.vram_free_mb - used_mb;
            } else {
                p.vram_bank_mb = p.vram_free_mb - used_mb;
                p.vram_experts = 0;
            }
        }
        if (p.vram_bank_mb > p.expert_total_mb) {
            p.vram_bank_mb = p.expert_total_mb;
        }
        // A bank below one full expert cycle thrashes: every token evicts what the next one needs.
        // Never ask for more than the model has — a 1.5 MB test model must still get a bank.
        const uint64_t floor_mb = std::min<uint64_t>(std::max<uint64_t>(model.cycle_floor_mb, 256),
                                                    std::max<uint64_t>(1, p.expert_total_mb));
        if (p.vram_bank_mb < floor_mb) {
            p.vram_bank_mb  = 0;
            p.vram_experts  = 0;
        }
    }
    p.expert_in_vram_mb = std::min(p.vram_bank_mb, p.expert_total_mb);

    // ---- 2. DDR4 ---------------------------------------------------------------------------------
    p.ram_free_mb    = mem.commit_avail_mb > 0 ? mem.commit_avail_mb
                     : (mem.ram_avail_mb > 0 ? mem.ram_avail_mb : 0);
    p.ram_reserve_mb = resolve_ram_reserve_mb(mem.ram_total_mb > 0 ? mem.ram_total_mb : p.ram_free_mb,
                                             (int64_t) cfg.ram_reserve_mb);
    // Dense weights that are NOT on the GPU still live in DDR4 next to the arena.
    p.dense_host_mb = (model.dense_bytes >> 20) > dense_gpu_mb ? (model.dense_bytes >> 20) - dense_gpu_mb : 0;

    // The experts that are still homeless after the VRAM pass. Byte-exact, because a single expert of
    // a small model is well under one MiB and rounding to whole MiB would push it to the disk.
    const uint64_t vram_experts_n  = std::min(p.vram_experts, max_experts);
    const uint64_t vram_bytes_used = vram_experts_n * per_expert_bytes;
    const uint64_t ram_left_of_experts = p.expert_total_mb - std::min(p.expert_total_mb,
                                          per_expert_bytes ? ceil_mb(vram_bytes_used) : p.expert_in_vram_mb);
    if (p.ram_free_mb > 0) {
        const uint64_t used_mb = p.ram_reserve_mb + p.dense_host_mb;
        if (p.ram_free_mb > used_mb) {
            const uint64_t budget_b = (p.ram_free_mb - used_mb) * kMiB;
            const uint64_t homeless = expert_total_bytes > vram_bytes_used ? expert_total_bytes - vram_bytes_used : 0;
            p.ram_experts = experts_fitting(budget_b, per_expert_bytes, max_experts - vram_experts_n);
            if (p.ram_experts == 0 && homeless > 0 && homeless <= budget_b) {
                p.ram_experts = max_experts - vram_experts_n; // everything that is left fits
            }
            const uint64_t ram_bytes = std::min<uint64_t>(homeless, p.ram_experts * per_expert_bytes);
            p.ram_arena_mb = ceil_mb(ram_bytes);
            p.expert_in_ram_mb = std::min(p.ram_arena_mb, ram_left_of_experts);
        }
    }
    if (p.ram_arena_mb == 0) p.ram_experts = 0;
    p.expert_on_disk_mb = p.expert_total_mb - p.expert_in_vram_mb - p.expert_in_ram_mb;

    // ---- 3. HDD ----------------------------------------------------------------------------------
    p.zero_hdd = p.expert_on_disk_mb == 0;
    p.silicon_pct = p.expert_total_mb > 0
        ? 100.0 * (double) (p.expert_in_vram_mb + p.expert_in_ram_mb) / (double) p.expert_total_mb
        : 0.0;

    // ---- rationale (Russian, the same strings the UI shows) --------------------------------------
    p.reasons.push_back("VRAM: свободно " + fmt_mb(p.vram_free_mb) +
                        " − KV " + fmt_mb(p.kv_mb) +
                        " − плотные веса на GPU " + fmt_mb(p.dense_gpu_mb) +
                        " − резерв " + fmt_mb(p.vram_reserve_mb) +
                        " = банк экспертов " + fmt_mb(p.vram_bank_mb));
    p.reasons.push_back("DDR4: свободно " + fmt_mb(p.ram_free_mb) +
                        " − резерв " + fmt_mb(p.ram_reserve_mb) +
                        " − плотные веса в ОЗУ " + fmt_mb(p.dense_host_mb) +
                        " = арена экспертов " + fmt_mb(p.ram_arena_mb));
    p.reasons.push_back("Эксперты: всего " + fmt_mb(p.expert_total_mb) +
                        " • в VRAM " + fmt_mb(p.expert_in_vram_mb) +
                        " • в ОЗУ " + fmt_mb(p.expert_in_ram_mb) +
                        " • с диска " + fmt_mb(p.expert_on_disk_mb));
    if (p.zero_hdd) {
        p.reasons.push_back("100% экспертов в VRAM + ОЗУ — в чате ноль чтений HDD");
    } else if (!p.reasons.empty()) {
        const bool disk_small = p.expert_on_disk_mb * 20 <= std::max<uint64_t>(1, p.expert_total_mb);
        p.reasons.push_back(disk_small
            ? fmt_gb((double) p.expert_on_disk_mb) + " экспертов остались на диске — это меньше 5% модели, "
              "после прогрева они подтянутся в освободившиеся слоты и займут один линейный проход HDD"
            : fmt_gb((double) p.expert_on_disk_mb) + " экспертов остались на диске: уменьшите резерв ОЗУ "
              "или VRAM, чтобы забрать их в память");
    }
    if (p.vram_bank_mb == 0 && ngl > 0 && p.expert_total_mb > 0) {
        p.reasons.push_back("VRAM после KV и резерва не остаётся на банк экспертов — эксперты пойдут "
                            "в DDR4 (CPU), внимание остаётся на GPU");
    }
    if (ngl == 0) {
        p.reasons.push_back("GPU-слоёв 0 (нет дискретной видеокарты или включён режим CPU) — "
                            "весь расчёт на процессоре, эксперты в DDR4");
    }

    p.ok = true;
    return p;
}

} // namespace dualmoe
