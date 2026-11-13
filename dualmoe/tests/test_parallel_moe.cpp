#include "engine.h"
#include "chat.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char ** argv) {
    if (argc < 2) {
        std::cerr << "Usage: dualmoe-tests <tiny_moe.gguf>\n";
        return 1;
    }
    const std::string model_path = argv[1];

    // Gate 1: GGUF zero-alloc MoE inspector
    const dualmoe::GgufLayout lay = dualmoe::inspect_gguf_layout(model_path);
    if (!lay.summary.ok || !lay.summary.is_moe || lay.summary.n_expert != 8 || lay.summary.n_expert_used != 2) {
        std::cerr << "[FAIL] Gate 1: GGUF MoE inspection failed: " << lay.summary.error << "\n";
        return 1;
    }
    std::cout << "[PASS] Gate 1: GGUF MoE layout verified (arch=" << lay.summary.arch
              << ", experts=" << lay.summary.n_expert << "x" << lay.summary.n_expert_used
              << ", expert_bytes=" << lay.summary.expert_bytes << ")\n";

    const std::string test_prompt = "Привет DualMoE работает параллельно на GPU и CPU";

    // Gate 2a: Resident baseline (no streaming)
    std::string baseline_out;
    {
        dualmoe::DualMoeEngine eng;
        dualmoe::EngineConfig cfg;
        cfg.model_path    = model_path;
        cfg.moe_stream    = false;
        cfg.zero_hdd_mode = false;
        cfg.n_gpu_layers  = 0;
        cfg.n_ctx         = 512;
        cfg.n_threads     = 2;
        std::string err;
        if (!eng.load_model(cfg, err)) {
            std::cerr << "[FAIL] Gate 2a load failed: " << err << "\n";
            return 1;
        }
        if (!eng.generate_raw(test_prompt, 24, 0.0f, 42, nullptr, baseline_out, err)) {
            std::cerr << "[FAIL] Gate 2a generate failed: " << err << "\n";
            return 1;
        }
    }

    // Gate 2b: Virtual-Arena On-Demand Streaming + Aggressive Eviction + release_mmap + >=2 GPU Layers Safe Split!
    std::string streamed_out;
    dualmoe::TelemetrySnapshot snap;
    {
        dualmoe::DualMoeEngine eng;
        dualmoe::EngineConfig cfg;
        cfg.model_path         = model_path;
        cfg.moe_stream         = true;
        cfg.zero_hdd_mode      = false;
        cfg.dense_anon         = true;
        cfg.release_mmap       = true;
        cfg.auto_split_experts = true;
        cfg.n_gpu_layers       = 4;
        cfg.vram_cache_mb      = 64;
        cfg.cpu_share          = 0.90f;
        cfg.ram_cache_mb       = 1;
        cfg.io_lanes           = 4;
        cfg.n_ctx              = 512;
        cfg.n_threads          = 2;

        std::string err;
        if (!eng.load_model(cfg, err)) {
            std::cerr << "[FAIL] Gate 2b streamed load failed: " << err << "\n";
            return 1;
        }
        if (!eng.generate_raw(test_prompt, 24, 0.0f, 42, nullptr, streamed_out, err)) {
            std::cerr << "[FAIL] Gate 2b streamed generate failed: " << err << "\n";
            return 1;
        }
        snap = eng.telemetry();
    }

    if (baseline_out != streamed_out) {
        std::cerr << "[FAIL] Gate 2: Byte-identity mismatch!\n"
                  << "  Resident: [" << baseline_out << "]\n"
                  << "  Streamed: [" << streamed_out << "]\n";
        return 1;
    }
    if (snap.ssd_expert_reads == 0) {
        std::cerr << "[FAIL] Gate 2: Expected >0 SSD expert reads in on-demand mode, got 0\n";
        return 1;
    }
    std::cout << "[PASS] Gate 2: On-Demand Streamed == Resident BYTE-IDENTICAL! (ssd_reads=" << snap.ssd_expert_reads
              << ", ram_hits=" << snap.ram_cache_hits
              << ", mmap_released=" << (snap.mmap_released ? "true" : "false") << ")\n";

    // Gate 3: Multi-turn Chat + Prefix KV Cache Reuse
    {
        dualmoe::DualMoeEngine eng;
        dualmoe::EngineConfig cfg;
        cfg.model_path           = model_path;
        cfg.moe_stream           = true;
        cfg.zero_hdd_mode        = true;
        cfg.ram_cache_mb         = 32;
        cfg.n_ctx                = 512;
        cfg.n_threads            = 2;
        cfg.sampling.temperature = 0.0f;
        cfg.sampling.max_tokens  = 12;

        std::string err, r1, r2;
        int32_t p1 = 0, c1 = 0, p2 = 0, c2 = 0;
        if (!eng.load_model(cfg, err)) {
            std::cerr << "[FAIL] Gate 3 load failed: " << err << "\n";
            return 1;
        }
        std::vector<dualmoe::ChatMessage> msgs = {
            {"system", "Ты наставник DualMoE"},
            {"user", "Привет!"},
        };
        if (!eng.generate_chat(msgs, cfg.sampling, false, nullptr, r1, p1, c1, err)) {
            std::cerr << "[FAIL] Gate 3 turn 1 failed: " << err << "\n";
            return 1;
        }
        msgs.push_back({"assistant", r1});
        msgs.push_back({"user", "Продолжай"});
        if (!eng.generate_chat(msgs, cfg.sampling, false, nullptr, r2, p2, c2, err)) {
            std::cerr << "[FAIL] Gate 3 turn 2 failed: " << err << "\n";
            return 1;
        }
        std::cout << "[PASS] Gate 3: Multi-turn Chat & Prefix KV Cache verified (c1=" << c1 << ", c2=" << c2 << ")\n";
    }

    // Gate 4: Zero-HDD Sequential VRAM <-> RAM Preload Mode (0 runtime disk reads + 100% byte-identity!)
    {
        dualmoe::DualMoeEngine eng;
        dualmoe::EngineConfig cfg;
        cfg.model_path           = model_path;
        cfg.moe_stream           = true;
        cfg.zero_hdd_mode        = true;
        cfg.auto_balance_compute = true;
        cfg.n_gpu_layers         = 99;
        cfg.vram_cache_mb        = 256;
        cfg.ram_cache_mb         = 64;
        cfg.io_lanes             = 1;
        cfg.n_ctx                = 512;
        cfg.n_threads            = 2;

        std::string err, zero_hdd_out;
        if (!eng.load_model(cfg, err)) {
            std::cerr << "[FAIL] Gate 4 Zero-HDD load failed: " << err << "\n";
            return 1;
        }
        if (!eng.generate_raw(test_prompt, 24, 0.0f, 42, nullptr, zero_hdd_out, err)) {
            std::cerr << "[FAIL] Gate 4 Zero-HDD generate failed: " << err << "\n";
            return 1;
        }
        const auto zsnap = eng.telemetry();
        if (zero_hdd_out != baseline_out) {
            std::cerr << "[FAIL] Gate 4: Zero-HDD output mismatch vs Resident!\n";
            return 1;
        }
        if (zsnap.ssd_expert_reads != 0) {
            std::cerr << "[FAIL] Gate 4: Expected 0 runtime HDD reads in Zero-HDD mode, got "
                      << zsnap.ssd_expert_reads << "\n";
            return 1;
        }
        if (!zsnap.zero_hdd_active || zsnap.silicon_residency_pct < 99.9) {
            std::cerr << "[FAIL] Gate 4: Expected 100% silicon residency, got "
                      << zsnap.silicon_residency_pct << "%\n";
            return 1;
        }
        std::cout << "[PASS] Gate 4: Zero-HDD Sequential Preload == Resident BYTE-IDENTICAL! "
                  << "(runtime_hdd_reads=" << zsnap.ssd_expert_reads
                  << ", silicon_residency=" << zsnap.silicon_residency_pct << "%"
                  << ", preload_bw=" << zsnap.preload_bandwidth_mbs << " MB/s)\n";
    }

    // Gate 5: Disjoint VRAM (+) RAM Multi-Projection Eviction, Rescue & 1-Layer Deferred Decommit Regression Test!
    // Directly guards against the Gemma 4 26B-A4B bug where ip=0 marked sibling projections (ffn_down_exps, ffn_up_exps)
    // kStateReady prematurely, causing ip>=1 to skip GPU->RAM readback and execute zero-filled pages!
    {
        const int32_t test_il = 0;
        const char * proj_names[3] = {
            "blk.0.ffn_gate_exps.weight",
            "blk.0.ffn_down_exps.weight",
            "blk.0.ffn_up_exps.weight",
        };
        std::vector<const dualmoe::GgufTensorLoc *> slices;
        for (const char * pname : proj_names) {
            auto it = lay.tensors.find(pname);
            if (it == lay.tensors.end()) {
                std::cerr << "[FAIL] Gate 5: Missing tensor " << pname << " in test GGUF\n";
                return 1;
            }
            slices.push_back(&it->second);
        }

        const int32_t n_exp = lay.summary.n_expert;
        auto bind_three_projs = [&](dualmoe::VirtualExpertArena & ar, ggml_context * gctx, std::string & aerr) -> bool {
            if (!ar.init(lay.summary.shard_paths, lay.summary.n_layer, n_exp,
                         /*ram_budget_bytes=*/16ull << 20, /*io_lanes=*/1, /*sequential_hdd_mode=*/true, aerr)) {
                return false;
            }
            for (int ip = 0; ip < 3; ++ip) {
                const uint64_t sb = slices[ip]->nbytes / (uint64_t) n_exp;
                ggml_tensor * t = ggml_new_tensor_3d(gctx, GGML_TYPE_I8, (int64_t) sb, 1, n_exp);
                ggml_set_name(t, proj_names[ip]);
                if (!ar.bind_layer_projection(test_il, t, slices[ip]->shard_idx, slices[ip]->file_offset, slices[ip]->nbytes, aerr)) {
                    return false;
                }
            }
            return true;
        };

        // First read ground-truth bytes for expert 0 and expert 1 across all 3 projections via normal RAM fetch
        std::vector<std::vector<uint8_t>> exp0_truth(3), exp1_truth(3);
        {
            ggml_context * ref_gctx = ggml_init({16 * 1024, nullptr, true});
            dualmoe::VirtualExpertArena ref_arena;
            std::string aerr;
            if (!bind_three_projs(ref_arena, ref_gctx, aerr)) {
                std::cerr << "[FAIL] Gate 5 ref_arena init failed: " << aerr << "\n";
                ggml_free(ref_gctx);
                return 1;
            }
            for (int ip = 0; ip < 3; ++ip) {
                const size_t sb = (size_t) (slices[ip]->nbytes / (uint64_t) n_exp);
                exp0_truth[ip].resize(sb);
                exp1_truth[ip].resize(sb);
                if (!ref_arena.fetch_expert_for_vram_prewarm(test_il, proj_names[ip], 0, exp0_truth[ip].data(), sb) ||
                    !ref_arena.fetch_expert_for_vram_prewarm(test_il, proj_names[ip], 1, exp1_truth[ip].data(), sb)) {
                    std::cerr << "[FAIL] Gate 5 ground-truth prewarm read failed\n";
                    ggml_free(ref_gctx);
                    return 1;
                }
            }
            ref_arena.shutdown();
            ggml_free(ref_gctx);
        }

        // Now run Disjoint VRAM (+) RAM cycle: prewarm -> evict all 3 projections -> verify non-null & byte-exact!
        ggml_context * disj_gctx = ggml_init({16 * 1024, nullptr, true});
        dualmoe::VirtualExpertArena disj_arena;
        std::string aerr;
        if (!bind_three_projs(disj_arena, disj_gctx, aerr)) {
            std::cerr << "[FAIL] Gate 5 disj_arena init failed: " << aerr << "\n";
            ggml_free(disj_gctx);
            return 1;
        }
        disj_arena.set_disjoint_vram_mode(true);

        // Prewarm expert 0 and expert 1 into simulated VRAM across all 3 projections
        for (int ip = 0; ip < 3; ++ip) {
            const size_t sb = (size_t) (slices[ip]->nbytes / (uint64_t) n_exp);
            std::vector<uint8_t> scratch(sb);
            if (!disj_arena.fetch_expert_for_vram_prewarm(test_il, proj_names[ip], 0, scratch.data(), sb) ||
                !disj_arena.fetch_expert_for_vram_prewarm(test_il, proj_names[ip], 1, scratch.data(), sb)) {
                std::cerr << "[FAIL] Gate 5 disjoint prewarm failed at ip=" << ip << "\n";
                ggml_free(disj_gctx);
                return 1;
            }
        }
        disj_arena.mark_preload_complete();

        // 5A: Evict expert 0 from VRAM -> DDR4 RAM across ALL 3 projections (ip = 0, 1, 2)
        void * evict_dsts[3] = {nullptr, nullptr, nullptr};
        for (int ip = 0; ip < 3; ++ip) {
            const size_t sb = (size_t) (slices[ip]->nbytes / (uint64_t) n_exp);
            evict_dsts[ip] = disj_arena.prepare_vram_eviction_dst(test_il, proj_names[ip], 0, sb);
            if (!evict_dsts[ip]) {
                std::cerr << "[FAIL] Gate 5A: prepare_vram_eviction_dst returned nullptr for projection ip="
                          << ip << " (" << proj_names[ip] << ") — multi-projection zeroing bug!\n";
                ggml_free(disj_gctx);
                return 1;
            }
            std::memcpy(evict_dsts[ip], exp0_truth[ip].data(), sb);
        }
        // Verify all 3 evicted projections match ground truth byte-for-byte with 0 runtime HDD reads!
        for (int ip = 0; ip < 3; ++ip) {
            if (std::memcmp(evict_dsts[ip], exp0_truth[ip].data(), exp0_truth[ip].size()) != 0) {
                std::cerr << "[FAIL] Gate 5A: Corrupted bytes in DDR4 after VRAM eviction at ip=" << ip << "\n";
                ggml_free(disj_gctx);
                return 1;
            }
        }

        // 5B: Rescue expert 1 onto CPU across all 3 projections while keeping m.in_vram = true,
        // then verify subsequent eviction of expert 1 STILL succeeds across all 3 projections!
        for (int ip = 0; ip < 3; ++ip) {
            const size_t sb = (size_t) (slices[ip]->nbytes / (uint64_t) n_exp);
            void * rdst = disj_arena.prepare_vram_rescue_dst(test_il, proj_names[ip], 1, sb);
            if (!rdst) {
                std::cerr << "[FAIL] Gate 5B: prepare_vram_rescue_dst returned nullptr at ip=" << ip << "\n";
                ggml_free(disj_gctx);
                return 1;
            }
            std::memcpy(rdst, exp1_truth[ip].data(), sb);
        }

        // 5C: Re-promote expert 0 to VRAM -> verify 1-layer deferred decommit holds DDR4 page until flush_deferred_promotions()
        const int32_t exp0_id = 0;
        disj_arena.on_experts_promoted_to_vram(test_il, &exp0_id, 1, /*n_evicted_to_ram=*/1);
        const uint64_t comm_before_flush = disj_arena.committed_bytes();
        disj_arena.flush_deferred_promotions();
        const uint64_t comm_after_flush = disj_arena.committed_bytes();
        if (comm_after_flush >= comm_before_flush) {
            std::cerr << "[FAIL] Gate 5C: Deferred promotion decommit did not release DDR4 pages on flush ("
                      << comm_before_flush << " -> " << comm_after_flush << ")\n";
            ggml_free(disj_gctx);
            return 1;
        }
        if (disj_arena.ssd_reads() != 0) {
            std::cerr << "[FAIL] Gate 5: Expected 0 HDD reads during Disjoint VRAM<->RAM swap, got "
                      << disj_arena.ssd_reads() << "\n";
            ggml_free(disj_gctx);
            return 1;
        }
        disj_arena.shutdown();
        ggml_free(disj_gctx);
        std::cout << "[PASS] Gate 5: Disjoint VRAM ⊕ RAM Multi-Projection Eviction + Rescue + Deferred Decommit verified "
                  << "(all 3 projections byte-exact, 0 HDD reads)!\n";
    }

    // Gate 6: Gemma 4 Official Jinja2 Chat Formatter + Past-Turn <|channel>thought...<channel|> Stripping!
    {
        const char * kGemma4Jinja =
            "{{- bos_token -}}"
            "{%- if enable_thinking is not defined or enable_thinking -%}"
            "<|turn>system\n<|think|><turn|>\n"
            "{%- endif -%}"
            "{%- for m in messages -%}"
            "{%- set role = 'model' if m['role'] == 'assistant' else m['role'] -%}"
            "<|turn>{{ role }}\n"
            "{% if m['role'] == 'assistant' %}<|channel>thought\n<channel|>{% endif %}"
            "{{ m['content'] }}<turn|>\n"
            "{%- endfor -%}"
            "{% if add_generation_prompt %}<|turn>model\n"
            "{% if enable_thinking is defined and not enable_thinking %}<|channel>thought\n<channel|>{% endif %}"
            "{% endif %}";

        auto tmpls = common_chat_templates_init(/*model=*/nullptr, kGemma4Jinja, "<bos>", "<turn|>");
        if (!tmpls || !common_chat_templates_was_explicit(tmpls.get())) {
            std::cerr << "[FAIL] Gate 6: Failed to initialize Gemma 4 Jinja2 template\n";
            return 1;
        }

        common_chat_templates_inputs in;
        in.use_jinja             = true;
        in.add_generation_prompt = true;
        in.enable_thinking       = true;
        in.add_bos               = true;
        {
            common_chat_msg u1; u1.role = "user"; u1.content = "Напиши про стек-трейс";
            common_chat_msg a1; a1.role = "assistant"; a1.reasoning_content = "Сначала разберём 0xC0000005"; a1.content = "Стек-трейс показывает цепочку вызовов.";
            common_chat_msg u2; u2.role = "user"; u2.content = "А что насчёт <turn|> внутри текста?";
            in.messages = {u1, a1, u2};
        }
        common_chat_params out_on = common_chat_templates_apply(tmpls.get(), in);
        const std::string rendered_on = out_on.prompt.str();
        if (rendered_on.find("<|im_start|>") != std::string::npos ||
            rendered_on.find("<|turn>system\n<|think|><turn|>") == std::string::npos ||
            rendered_on.find("Сначала разберём 0xC0000005") != std::string::npos ||
            rendered_on.find("<bos><bos>") != std::string::npos) {
            std::cerr << "[FAIL] Gate 6: Unexpected Gemma 4 Jinja2 output (thinking=ON):\n" << rendered_on << "\n";
            return 1;
        }

        // Verify user message part is marked is_input=true so user text cannot inject fake <|turn> tokens!
        bool saw_protected_input = false;
        for (const auto & p : out_on.prompt.parts) {
            if (p.is_input && p.text.find("<turn|>") != std::string::npos) {
                saw_protected_input = true;
            }
        }
        if (!saw_protected_input) {
            std::cerr << "[FAIL] Gate 6: Expected user input containing <turn|> to have part.is_input == true\n";
            return 1;
        }

        in.enable_thinking = false;
        common_chat_params out_off = common_chat_templates_apply(tmpls.get(), in);
        const std::string rendered_off = out_off.prompt.str();
        if (rendered_off.rfind("<|turn>model\n<|channel>thought\n<channel|>") == std::string::npos) {
            std::cerr << "[FAIL] Gate 6: Expected suppressed thought block when enable_thinking=false:\n"
                      << rendered_off << "\n";
            return 1;
        }
        std::cout << "[PASS] Gate 6: Gemma 4 Jinja2 Chat Template + Single-BOS + Past-Thought Stripping verified!\n";
    }

    std::cout << "ALL DUALMOE v0.4.0 GATES PASSED!\n";
    return 0;
}
