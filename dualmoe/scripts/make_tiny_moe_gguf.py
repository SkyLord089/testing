#!/usr/bin/env python3
"""Generates a valid miniature MoE .gguf model (qwen3moe) for automated byte-identity & live server testing."""
import pathlib
import sys
import numpy as np
import gguf


def build_tiny_moe(out_path: pathlib.Path) -> None:
    arch = "qwen3moe"
    writer = gguf.GGUFWriter(str(out_path), arch)

    n_layer = 4
    n_embd = 64
    n_head = 2
    n_head_kv = 2
    head_dim = n_embd // n_head  # 32 (divisible by Q8_0 block size 32)
    n_ff_exp = 128
    n_expert = 8
    n_expert_used = 2

    # Build complete SPM vocabulary: 6 control + 256 <0xXX> byte fallback + Russian/tech words = 320 tokens
    control_pieces = ["<pad>", "<bos>", "<eos>", "<unk>", "<|im_start|>", "<|im_end|>"]
    byte_pieces = [f"<0x{b:02X}>" for b in range(256)]
    word_pieces = [
        "system", "user", "assistant", "\n", "▁",
        "▁DualMoE", "▁работает", "▁параллельно", "▁на", "▁GPU", "▁и", "▁CPU", "▁со",
        "▁стримингом", "▁экспертов", "▁из", "▁SSD", "▁в", "▁память", "▁DDR4", "▁VRAM",
        "▁Привет", "!", ",", ".", ":", "▁—", "▁быстро", "▁без", "▁вылетов", "▁слоёв",
        "▁модель", "▁готова", "▁к", "▁обучению", "▁LearnHarness", "▁Vulkan", "▁RX580", "▁Ryzen",
        "▁поток", "▁кэш", "▁скорость", "▁токен", "▁отлично", "▁тест", "▁пройден",
    ]
    tokens = list(control_pieces) + byte_pieces + list(word_pieces)
    idx = 0
    while len(tokens) < 320:
        tokens.append(f"▁слово_{idx}")
        idx += 1

    scores = []
    tok_types = []
    for i, _ in enumerate(tokens):
        if i == 3:
            tok_types.append(gguf.TokenType.UNKNOWN)
            scores.append(0.0)
        elif i < 6:
            tok_types.append(gguf.TokenType.CONTROL)
            scores.append(0.0)
        elif i < 6 + 256:
            tok_types.append(gguf.TokenType.BYTE)
            scores.append(-100.0)
        else:
            tok_types.append(gguf.TokenType.NORMAL)
            scores.append(-float(i - 262))

    writer.add_name("DualMoE-Tiny-MoE-8xTop2-Test")
    writer.add_context_length(2048)
    writer.add_embedding_length(n_embd)
    writer.add_block_count(n_layer)
    writer.add_feed_forward_length(n_ff_exp * n_expert_used)
    writer.add_uint32(f"{arch}.expert_feed_forward_length", n_ff_exp)
    writer.add_head_count(n_head)
    writer.add_head_count_kv(n_head_kv)
    writer.add_uint32(f"{arch}.attention.key_length", head_dim)
    writer.add_uint32(f"{arch}.attention.value_length", head_dim)
    writer.add_layer_norm_rms_eps(1e-6)
    writer.add_rope_freq_base(10000.0)
    writer.add_expert_count(n_expert)
    writer.add_expert_used_count(n_expert_used)
    writer.add_chat_template(
        "{% for message in messages %}{{'<|im_start|>' + message['role'] + '\n' + message['content'] + '<|im_end|>' + '\n'}}{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\n' }}{% endif %}"
    )

    writer.add_tokenizer_model("llama")
    writer.add_token_list(tokens)
    writer.add_token_scores(scores)
    writer.add_token_types(tok_types)
    writer.add_bos_token_id(1)
    writer.add_eos_token_id(2)
    writer.add_pad_token_id(0)
    writer.add_unk_token_id(3)

    rng = np.random.default_rng(20261111)

    def rand_f32(shape_np, scale=0.08):
        return (rng.standard_normal(shape_np) * scale).astype(np.float32)

    def rand_f16(shape_np, scale=0.08):
        return (rng.standard_normal(shape_np) * scale).astype(np.float16)

    # Note: numpy shape is reversed vs ggml ne: ggml {ne0, ne1, ne2} == numpy (ne2, ne1, ne0)
    writer.add_tensor("token_embd.weight", rand_f16((len(tokens), n_embd)))
    writer.add_tensor("output_norm.weight", np.ones((n_embd,), dtype=np.float32))
    out_w = rand_f16((len(tokens), n_embd), scale=0.005)
    out_w[267:] = rand_f16((len(tokens) - 267, n_embd), scale=0.35)
    writer.add_tensor("output.weight", out_w)

    n_embd_k_gqa = head_dim * n_head_kv
    for il in range(n_layer):
        writer.add_tensor(f"blk.{il}.attn_norm.weight", np.ones((n_embd,), dtype=np.float32))
        writer.add_tensor(f"blk.{il}.attn_q.weight", rand_f16((n_embd, n_embd)))
        writer.add_tensor(f"blk.{il}.attn_k.weight", rand_f16((n_embd_k_gqa, n_embd)))
        writer.add_tensor(f"blk.{il}.attn_v.weight", rand_f16((n_embd_k_gqa, n_embd)))
        writer.add_tensor(f"blk.{il}.attn_output.weight", rand_f16((n_embd, n_embd)))
        writer.add_tensor(f"blk.{il}.attn_q_norm.weight", np.ones((head_dim,), dtype=np.float32))
        writer.add_tensor(f"blk.{il}.attn_k_norm.weight", np.ones((head_dim,), dtype=np.float32))

        writer.add_tensor(f"blk.{il}.ffn_norm.weight", np.ones((n_embd,), dtype=np.float32))
        writer.add_tensor(f"blk.{il}.ffn_gate_inp.weight", rand_f32((n_expert, n_embd), scale=0.4))

        # Distinct per-expert weights so routing & streaming errors are immediately caught by byte-identity gates!
        writer.add_tensor(f"blk.{il}.ffn_gate_exps.weight", rand_f16((n_expert, n_ff_exp, n_embd), scale=0.12))
        writer.add_tensor(f"blk.{il}.ffn_up_exps.weight", rand_f16((n_expert, n_ff_exp, n_embd), scale=0.12))
        writer.add_tensor(f"blk.{il}.ffn_down_exps.weight", rand_f16((n_expert, n_embd, n_ff_exp), scale=0.12))

    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()


if __name__ == "__main__":
    target = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "tiny_moe.gguf")
    target.parent.mkdir(parents=True, exist_ok=True)
    build_tiny_moe(target)
    print(f"Created {target} ({target.stat().st_size} bytes)")
