#include "gguf_inspect.h"

#include "gguf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

namespace dualmoe {

namespace {

int64_t read_kv_int(const gguf_context * ctx, const std::string & key, int64_t fallback = 0) {
    const int64_t id = gguf_find_key(ctx, key.c_str());
    if (id < 0) return fallback;
    switch (gguf_get_kv_type(ctx, id)) {
        case GGUF_TYPE_UINT8:  return gguf_get_val_u8(ctx, id);
        case GGUF_TYPE_INT8:   return gguf_get_val_i8(ctx, id);
        case GGUF_TYPE_UINT16: return gguf_get_val_u16(ctx, id);
        case GGUF_TYPE_INT16:  return gguf_get_val_i16(ctx, id);
        case GGUF_TYPE_UINT32: return gguf_get_val_u32(ctx, id);
        case GGUF_TYPE_INT32:  return gguf_get_val_i32(ctx, id);
        case GGUF_TYPE_UINT64: return (int64_t) gguf_get_val_u64(ctx, id);
        case GGUF_TYPE_INT64:  return gguf_get_val_i64(ctx, id);
        case GGUF_TYPE_ARRAY: {
            // Some hybrid/MoE architectures store per-layer expert_used_count as an integer array
            const size_t n = gguf_get_arr_n(ctx, id);
            if (n == 0) return fallback;
            const void * data = gguf_get_arr_data(ctx, id);
            const auto   at   = gguf_get_arr_type(ctx, id);
            int64_t max_v = 0;
            for (size_t i = 0; i < n; ++i) {
                int64_t v = 0;
                if (at == GGUF_TYPE_UINT32) v = ((const uint32_t *) data)[i];
                else if (at == GGUF_TYPE_INT32) v = ((const int32_t *) data)[i];
                else if (at == GGUF_TYPE_INT64) v = ((const int64_t *) data)[i];
                if (v > max_v) max_v = v;
            }
            return max_v > 0 ? max_v : fallback;
        }
        default: return fallback;
    }
}

std::string read_kv_str(const gguf_context * ctx, const std::string & key, const std::string & fallback = "") {
    const int64_t id = gguf_find_key(ctx, key.c_str());
    if (id < 0 || gguf_get_kv_type(ctx, id) != GGUF_TYPE_STRING) return fallback;
    const char * s = gguf_get_val_str(ctx, id);
    return s ? std::string(s) : fallback;
}

int32_t parse_block_layer(const std::string & name) {
    if (name.compare(0, 4, "blk.") != 0) return -1;
    const size_t dot = name.find('.', 4);
    if (dot == std::string::npos || dot == 4) return -1;
    char * endp = nullptr;
    const long v = std::strtol(name.c_str() + 4, &endp, 10);
    if (endp != name.c_str() + dot || v < 0) return -1;
    return (int32_t) v;
}

bool is_routed_expert_name(const std::string & name) {
    return name.find("_exps.") != std::string::npos;
}

std::string path_to_utf8(const fs::path & p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

fs::path utf8_to_path(const std::string & s) {
    return fs::u8path(s);
}

std::vector<std::string> discover_shards(const std::string & first_path) {
    static const std::regex shard_re(R"(^(.*)-(\d{5})-of-(\d{5})\.gguf$)", std::regex::icase);
    std::smatch m;
    if (std::regex_match(first_path, m, shard_re)) {
        const std::string prefix = m[1].str();
        const int total = std::atoi(m[3].str().c_str());
        if (total > 1 && total <= 256) {
            std::vector<std::string> out;
            out.reserve(total);
            for (int i = 1; i <= total; ++i) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "-%05d-of-%05d.gguf", i, total);
                const std::string candidate = prefix + buf;
                std::error_code ec;
                if (!fs::exists(utf8_to_path(candidate), ec)) {
                    return {first_path};
                }
                out.push_back(candidate);
            }
            return out;
        }
    }
    return {first_path};
}

} // namespace

GgufLayout inspect_gguf_layout(const std::string & model_path) {
    GgufLayout out;
    ModelSummary & s = out.summary;
    s.path = model_path;
    const fs::path fsp = utf8_to_path(model_path);
    s.filename = path_to_utf8(fsp.filename());

    std::error_code ec;
    if (!fs::exists(fsp, ec)) {
        s.error = "Файл модели не найден: " + model_path;
        return out;
    }

    s.shard_paths = discover_shards(model_path);
    for (const std::string & sp : s.shard_paths) {
        s.file_size_bytes += fs::file_size(utf8_to_path(sp), ec);
    }

    gguf_init_params params = { /*.no_alloc =*/ true, /*.ctx =*/ nullptr };
    gguf_context * ctx0 = gguf_init_from_file(s.shard_paths[0].c_str(), params);
    if (!ctx0) {
        s.error = "Не удалось прочитать заголовок GGUF: " + s.shard_paths[0];
        return out;
    }

    s.arch = read_kv_str(ctx0, "general.architecture");
    s.model_name = read_kv_str(ctx0, "general.name", s.filename);
    if (!s.arch.empty()) {
        s.n_layer       = (int32_t) read_kv_int(ctx0, s.arch + ".block_count", 0);
        s.n_embd        = (int32_t) read_kv_int(ctx0, s.arch + ".embedding_length", 0);
        s.n_expert      = (int32_t) read_kv_int(ctx0, s.arch + ".expert_count", 0);
        s.n_expert_used = (int32_t) read_kv_int(ctx0, s.arch + ".expert_used_count", 0);
        s.n_ctx_train   = (int32_t) read_kv_int(ctx0, s.arch + ".context_length", 4096);
    }
    gguf_free(ctx0);

    if (s.n_layer > 0) {
        out.layer_expert_names.resize((size_t) s.n_layer);
    }

    for (size_t si = 0; si < s.shard_paths.size(); ++si) {
        gguf_context * ctx = gguf_init_from_file(s.shard_paths[si].c_str(), params);
        if (!ctx) {
            s.error = "Ошибка чтения шарда GGUF: " + s.shard_paths[si];
            return out;
        }
        const uint64_t data_off = (uint64_t) gguf_get_data_offset(ctx);
        const int64_t n_tensors = gguf_get_n_tensors(ctx);
        for (int64_t ti = 0; ti < n_tensors; ++ti) {
            GgufTensorLoc loc;
            loc.name        = gguf_get_tensor_name(ctx, ti);
            loc.shard_idx   = (int32_t) si;
            loc.file_offset = data_off + (uint64_t) gguf_get_tensor_offset(ctx, ti);
            loc.nbytes      = (uint64_t) gguf_get_tensor_size(ctx, ti);
            const int64_t * ne = gguf_get_tensor_ne(ctx, ti);
            for (int d = 0; d < 4; ++d) loc.ne[d] = ne[d];
            loc.layer_idx   = parse_block_layer(loc.name);

            if (s.n_expert >= 2 && loc.layer_idx >= 0 && loc.ne[2] == s.n_expert && loc.ne[3] == 1 &&
                is_routed_expert_name(loc.name)) {
                loc.is_expert = true;
                s.expert_bytes += loc.nbytes;
                if ((size_t) loc.layer_idx >= out.layer_expert_names.size()) {
                    out.layer_expert_names.resize((size_t) loc.layer_idx + 1);
                }
                out.layer_expert_names[(size_t) loc.layer_idx].push_back(loc.name);
            } else {
                s.dense_bytes += loc.nbytes;
            }
            out.tensors.emplace(loc.name, std::move(loc));
        }
        gguf_free(ctx);
    }

    s.is_moe = (s.n_expert >= 2 && s.expert_bytes > 0);
    if (s.is_moe) {
        const int32_t k = std::max<int32_t>(1, s.n_expert_used > 0 ? s.n_expert_used : 2);
        uint64_t cycle_bytes = 0;
        for (const auto & names : out.layer_expert_names) {
            for (const std::string & tn : names) {
                const auto & loc = out.tensors.at(tn);
                const uint64_t per_exp = loc.nbytes / (uint64_t) s.n_expert;
                cycle_bytes += per_exp * (uint64_t) std::min<int32_t>(k, s.n_expert);
            }
        }
        s.cycle_floor_mb = (cycle_bytes + (1ull << 20) - 1) >> 20;
    }

    s.ok = true;
    return out;
}

std::vector<ModelSummary> scan_gguf_directory(const std::string & dir_path) {
    std::vector<ModelSummary> results;
    std::error_code ec;
    const fs::path root = utf8_to_path(dir_path);
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        return results;
    }

    static const std::regex non_first_shard(R"(^.*-(\d{5})-of-\d{5}\.gguf$)", std::regex::icase);
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) continue;
        const std::string p   = path_to_utf8(it->path());
        const std::string ext = path_to_utf8(it->path().extension());
        if (ext != ".gguf" && ext != ".GGUF") continue;
        std::smatch m;
        if (std::regex_match(p, m, non_first_shard) && std::atoi(m[1].str().c_str()) > 1) {
            continue;
        }
        GgufLayout lay = inspect_gguf_layout(p);
        if (lay.summary.ok) {
            results.push_back(std::move(lay.summary));
        }
    }

    std::sort(results.begin(), results.end(), [](const ModelSummary & a, const ModelSummary & b) {
        return a.filename < b.filename;
    });
    return results;
}

} // namespace dualmoe
