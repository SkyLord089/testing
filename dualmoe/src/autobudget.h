// DualMoE AutoBudget — "занять всё, но не тронуть резерв".
//
// The loader, the benchmark plan and the UI all need the same answer to one question:
//   *how many expert bytes may I put into VRAM, and how many into DDR4, given what the
//    system must keep free for itself?*
//
// Priority order is fixed and matches the engine's own design:
//   1. VRAM  — attention + KV + as many experts as fit in the bank the GPU can hold;
//   2. DDR4  — every remaining expert until the reserve the user wants to keep is reached;
//   3. HDD   — only the experts that did not fit into (1) or (2).
//
// The reserve the user picks is subtracted from *what is free right now*, never from total
// RAM/VRAM, so a browser tab or another model already resident is accounted for.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dualmoe/types.h"

namespace dualmoe {

// Where the numbers came from — shown in the UI so the user can see it is arithmetics, not magic.
struct AutoBudgetPlan {
    bool        ok = false;
    std::string error;

    // Inputs (all MiB)
    uint64_t vram_free_mb      = 0;   // free VRAM reported by the driver
    uint64_t kv_mb             = 0;   // KV cache + graph that must stay on the GPU
    uint64_t dense_gpu_mb      = 0;   // dense weights offloaded to the GPU
    uint64_t vram_bank_mb      = 0;   // experts that fit into VRAM
    uint64_t vram_reserve_mb   = 0;   // the reserve the user asked to keep free

    uint64_t ram_free_mb       = 0;   // available DDR4 / commit
    uint64_t ram_reserve_mb    = 0;   // the reserve the user asked to keep free
    uint64_t ram_arena_mb      = 0;   // experts that fit into DDR4
    uint64_t dense_host_mb     = 0;   // dense weights that stay in DDR4

    uint64_t expert_total_mb   = 0;   // all routed experts of the model
    uint64_t expert_in_vram_mb = 0;
    uint64_t expert_in_ram_mb  = 0;
    uint64_t expert_on_disk_mb = 0;
    double   silicon_pct       = 0.0; // share of the experts that live in VRAM + DDR4

    // Expert granularity: budgets must be a whole number of experts.
    uint64_t per_expert_mb      = 0;  // smallest expert of the model (indivisible step)
    uint64_t vram_experts       = 0;  // expert slots that fit (approximate, uniform per layer)
    uint64_t ram_experts        = 0;
    uint64_t total_experts      = 0;

    bool     zero_hdd          = false; // every expert fits into VRAM + DDR4
    std::vector<std::string> reasons;
};

// Reserves given in MiB. -1 means "automatic" (see resolve_reserve_*), 0 means "use everything".
uint64_t resolve_vram_reserve_mb(uint64_t vram_total_mb, int64_t requested_mb);
uint64_t resolve_ram_reserve_mb(uint64_t ram_total_mb, int64_t requested_mb);

// KV-cache footprint of the GPU-resident part of the model (SWA + shared-KV aware), in MiB.
// Shared by the loader and the benchmark plan so both size the expert bank against the same number.
uint64_t estimate_kv_mb(const ModelSummary & model, const EngineConfig & cfg, int32_t ngl);

// The whole calculation. Pure function of the model + the memory situation, no global state,
// so the same numbers appear in the loader, the benchmark plan and the UI.
AutoBudgetPlan plan_auto_budget(const ModelSummary & model,
                                const SystemMemoryInfo & mem,
                                const EngineConfig & cfg,
                                int32_t ngl,
                                uint64_t kv_mb,
                                uint64_t dense_gpu_mb);

} // namespace dualmoe
