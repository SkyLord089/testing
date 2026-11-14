// DualMoE speculative lookahead ("Разгон").
//
// The decode loop is bound by expert selection: a single token touches only its own
// top-k experts, so every model weight transfer and every cache miss is paid per token.
// A batch of B candidate tokens, however, is verified by ONE graph with the same
// top-k*B expert reads — so accepting even 2 of 4 drafted tokens is a ~2x win.
//
// The drafter needs no second model: it looks the continuation up in the token history
// (the prompt, the answer being written, and the expert warm-state file of the model,
// which already stores what this model tends to route/say next for the same context).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dualmoe {

struct SpecStats {
    uint64_t drafted    = 0;   // tokens proposed by the drafter
    uint64_t accepted   = 0;   // drafted tokens that the model confirmed
    uint64_t rounds     = 0;   // verify rounds (one graph each)
    uint64_t saved      = 0;   // accepted tokens that cost no extra graph
    double   efficiency = 1.0; // accepted / rounds, > 1 means faster than plain decode
};

// n-gram -> empirical distribution of the tokens that followed it, built from the token
// history of this model and (optionally) persisted next to it, so the speed-up starts on
// the very first answer after a restart instead of learning for minutes.
class SpecDrafter {
public:
    void clear();
    bool enabled() const { return n_max_ > 0; }
    void set_n_max(int32_t n) { n_max_ = n < 0 ? 0 : n; }
    int32_t n_max() const { return n_max_; }

    // Append new tokens to the history (inp_data must only ever grow, see n_prev).
    void observe(const std::vector<int32_t> & tokens);

    // Draft up to n_max tokens that most likely follow `context` (context already ends with
    // the last sampled token). Returns an empty vector when nothing matches.
    std::vector<int32_t> draft(const std::vector<int32_t> & context) const;

    bool save(const std::string & path) const;
    bool load(const std::string & path);

    const SpecStats & stats() const { return stats_; }

    // Self-tuning guard: a verify round only pays off when it yields more than one token. If the
    // measured efficiency stays below the threshold, speculation is disabled for this answer, so a
    // compute-bound machine never gets slower than plain decode.
    bool effective() const {
        if (!enabled()) return false;
        if (stats_.rounds < 4) return true;                 // give the drafter a fair trial
        return stats_.efficiency >= kMinEfficiency;
    }
    // A verify graph costs about as much as a plain graph (an MoE batch pays for its expert rows,
    // not for the whole bank), so the break-even is ~1.1 tokens per graph. Anything above is profit.
    static constexpr double kMinEfficiency = 1.10;
    void reset_stats() { stats_ = SpecStats{}; }
    void add_drafts(uint64_t n)      { stats_.drafted += n; stats_.rounds += 1; }
    void add_accepted(uint64_t n)    { stats_.accepted += n; stats_.saved += n; }
    // Tokens produced per graph: one plain token per round plus every confirmed draft.
    void recompute_efficiency() {
        stats_.efficiency = stats_.rounds > 0
            ? (double) (stats_.rounds + stats_.saved) / (double) stats_.rounds : 1.0;
    }

private:
    static constexpr int32_t kMaxOrder = 4;   // longest n-gram looked up
    static uint32_t hash_ngram(const std::vector<int32_t> & t, size_t end, int32_t order);
    void insert_ngrams(const std::vector<int32_t> & t, size_t from, size_t to);

    int32_t n_max_ = 0;
    size_t  n_seen_ = 0;
    std::vector<int32_t> history_;
    std::unordered_map<uint32_t, std::unordered_map<int32_t, uint32_t>> table_;
    SpecStats stats_;
};

} // namespace dualmoe
