#include "spec.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dualmoe {

namespace {

// The drafter must stay cheap: it runs once per token, on the main generation thread.
constexpr size_t kMaxTableEntries = 200000;
constexpr size_t kMaxHistory      = 32768;

FILE * open_utf8(const std::string & path, const char * mode) {
    return std::fopen(path.c_str(), mode);
}

} // namespace

void SpecDrafter::clear() {
    history_.clear();
    table_.clear();
    n_seen_ = 0;
}

uint32_t SpecDrafter::hash_ngram(const std::vector<int32_t> & t, size_t end, int32_t order) {
    // FNV-1a over the last `order` tokens before `end`
    uint32_t h = 2166136261u;
    const size_t begin = end >= (size_t) order ? end - (size_t) order : 0;
    for (size_t i = begin; i < end; ++i) {
        h ^= (uint32_t) t[i];
        h *= 16777619u;
    }
    return h;
}

void SpecDrafter::insert_ngrams(const std::vector<int32_t> & t, size_t from, size_t to) {
    if (table_.size() >= kMaxTableEntries) return;
    // The n-gram ending just before position i is followed by the token at i itself, so the table
    // maps "the last `order` tokens" -> "the token that came next".
    for (size_t i = from; i < to && i < t.size(); ++i) {
        const int32_t next = t[i];
        for (int32_t order = 2; order <= kMaxOrder; ++order) {
            if (i < (size_t) order) break;
            const uint32_t h = hash_ngram(t, i, order);
            auto & cnt = table_[h][next];
            if (cnt < 255) ++cnt;
        }
    }
}

void SpecDrafter::observe(const std::vector<int32_t> & tokens) {
    // Token histories only ever grow within one engine run, so only the tail is new. A shorter
    // history means the context was reset (new prompt): rebuild, but keep the learned table —
    // the 0.5-2 GB model's own phrasing stays useful across prompts.
    size_t from = n_seen_;
    if (tokens.size() < n_seen_) {
        history_.clear();
        from = 0;
    }
    if (tokens.size() <= from && !history_.empty()) {
        history_ = tokens;
        return;
    }
    // Merge instead of copying the caller's buffer: the in-flight history keeps growing token by token,
    // and copying it on every round would cost more than the drafting itself.
    const size_t overlap = std::min(history_.size(), from);
    if (overlap > 0 && tokens.size() >= overlap) {
        const bool same_prefix = std::equal(history_.begin(), history_.begin() + (long) overlap,
                                            tokens.begin());
        if (!same_prefix) {
            history_.clear();
            from = 0;
        }
    } else if (overlap == 0) {
        history_.clear();
        from = 0;
    }
    if (tokens.size() > from) {
        history_.insert(history_.end(), tokens.begin() + (long) from, tokens.end());
    }
    insert_ngrams(history_, from, history_.size());
    n_seen_ = history_.size();

    if (history_.size() > kMaxHistory) {
        // Keep the tail; the table stays (it only holds hashes and counts).
        history_.erase(history_.begin(), history_.end() - (long) kMaxHistory);
        n_seen_ = history_.size();
    }
}

std::vector<int32_t> SpecDrafter::draft(const std::vector<int32_t> & context) const {
    std::vector<int32_t> out;
    if (n_max_ <= 0 || context.size() < 2) return out;

    // Try the longest context match first (much higher acceptance), fall back to shorter ones.
    std::vector<int32_t> best;
    for (int32_t order = kMaxOrder; order >= 2; --order) {
        if (context.size() < (size_t) order) continue;
        const uint32_t h = hash_ngram(context, context.size(), order);
        const auto it = table_.find(h);
        if (it == table_.end()) continue;

        // Pick the most frequent continuation, then follow the chain inside the same bucket.
        std::vector<std::pair<uint32_t, int32_t>> ranked(it->second.begin(), it->second.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto & a, const auto & b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        std::vector<int32_t> cand;
        int32_t last = ranked.front().first;
        cand.push_back(last);
        for (int32_t step = 1; step < n_max_; ++step) {
            // The drafted tokens are the newest history: extend the context with them and look the
            // trailing n-gram up again.
            std::vector<int32_t> probe = context;
            probe.insert(probe.end(), cand.begin(), cand.end());
            const uint32_t h2 = hash_ngram(probe, probe.size(), order);
            const auto it2 = table_.find(h2);
            if (it2 == table_.end()) break;
            int32_t nxt = -1;
            uint32_t best_cnt = 0;
            for (const auto & kv : it2->second) {
                if (kv.second > best_cnt) { best_cnt = kv.second; nxt = kv.first; }
            }
            if (nxt < 0 || best_cnt < 2) break; // require at least two sightings to trust a chain
            cand.push_back(nxt);
        }
        if (cand.size() > best.size()) best = std::move(cand);
        if (best.size() >= (size_t) n_max_) break;
    }
    if ((int32_t) best.size() > n_max_) best.resize((size_t) n_max_);
    // A single drafted token cannot beat plain decode (it costs a full graph anyway).
    if (best.size() < 2) best.clear();
    return best;
}

bool SpecDrafter::save(const std::string & path) const {
    if (table_.empty()) return false;
    FILE * f = open_utf8(path, "wb");
    if (!f) return false;
    const char magic[8] = {'D', 'M', 'S', 'P', 'E', 'C', '0', '1'};
    std::fwrite(magic, 1, sizeof(magic), f);
    const uint32_t n_hist = (uint32_t) std::min(history_.size(), kMaxHistory);
    std::fwrite(&n_hist, sizeof(n_hist), 1, f);
    std::fwrite(history_.data() + (history_.size() - n_hist), sizeof(int32_t), n_hist, f);

    uint32_t n_entries = 0;
    for (const auto & kv : table_) n_entries += (uint32_t) kv.second.size();
    std::fwrite(&n_entries, sizeof(n_entries), 1, f);
    for (const auto & kv : table_) {
        for (const auto & tok : kv.second) {
            const uint32_t h = kv.first;
            const int32_t  t = tok.first;
            const uint32_t c = tok.second;
            std::fwrite(&h, sizeof(h), 1, f);
            std::fwrite(&t, sizeof(t), 1, f);
            std::fwrite(&c, sizeof(c), 1, f);
        }
    }
    std::fclose(f);
    return true;
}

bool SpecDrafter::load(const std::string & path) {
    FILE * f = open_utf8(path, "rb");
    if (!f) return false;
    char magic[8] = {0};
    if (std::fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
        std::memcmp(magic, "DMSPEC01", 8) != 0) {
        std::fclose(f);
        return false;
    }
    uint32_t n_hist = 0;
    if (std::fread(&n_hist, sizeof(n_hist), 1, f) != 1 || n_hist > kMaxHistory) {
        std::fclose(f);
        return false;
    }
    history_.assign((size_t) n_hist, 0);
    if (n_hist && std::fread(history_.data(), sizeof(int32_t), n_hist, f) != n_hist) {
        std::fclose(f);
        history_.clear();
        return false;
    }
    n_seen_ = history_.size();

    uint32_t n_entries = 0;
    if (std::fread(&n_entries, sizeof(n_entries), 1, f) != 1) {
        std::fclose(f);
        return false;
    }
    table_.clear();
    for (uint32_t i = 0; i < n_entries && i < kMaxTableEntries * 4; ++i) {
        uint32_t h = 0, c = 0;
        int32_t  t = 0;
        if (std::fread(&h, sizeof(h), 1, f) != 1 ||
            std::fread(&t, sizeof(t), 1, f) != 1 ||
            std::fread(&c, sizeof(c), 1, f) != 1) {
            break;
        }
        if (c == 0 || c > 255) continue;
        table_[h][t] = c;
    }
    std::fclose(f);
    return !table_.empty();
}

} // namespace dualmoe
