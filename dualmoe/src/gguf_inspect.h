#pragma once

#include "dualmoe/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dualmoe {

struct GgufTensorLoc {
    std::string name;
    int32_t     shard_idx   = 0;
    uint64_t    file_offset = 0;   // Absolute byte offset inside shard_paths[shard_idx]
    uint64_t    nbytes      = 0;   // Total tensor byte size
    int64_t     ne[4]       = {1, 1, 1, 1};
    int32_t     layer_idx   = -1;  // >= 0 for blk.<il>.* tensors
    bool        is_expert   = false;
};

struct GgufLayout {
    ModelSummary summary;
    std::unordered_map<std::string, GgufTensorLoc> tensors;
    std::vector<std::vector<std::string>> layer_expert_names; // [n_layer] list of routed expert tensor names
};

GgufLayout inspect_gguf_layout(const std::string & model_path);
std::vector<ModelSummary> scan_gguf_directory(const std::string & dir_path);

} // namespace dualmoe
