// schema/heuristic_inference.h - deterministic rules that run before the AI is
// ever called (spec 4.4).
#pragma once

#include "data_profiler.h"
#include "schema.h"

#include <string>
#include <vector>

namespace merope {

// Below this, the proposal is downgraded to UNKNOWN rather than shown as fact.
inline constexpr double k_min_semantic_confidence = 0.50;

struct inference_hint_t {
    std::size_t     physical_index = 0;
    std::string     semantic_name;
    semantic_type_t semantic_type = semantic_type_t::unknown;
    double          confidence    = 0.0;
    std::string     rationale;

    bool            proposed      = true;
};

struct heuristic_result_t {
    std::vector<inference_hint_t> hints;  // one entry per physical column
};

heuristic_result_t infer_heuristically(const dataset_profile_t& profile);

// Builds the unified schema: physical facts from the profile, semantic layer
// from the hints. Nothing is marked user_confirmed here.
schema_t build_schema(const dataset_profile_t& profile, const heuristic_result_t& hints);

// Overlays a set of hints (typically the AI proposal) onto an existing schema,
// leaving columns the user already confirmed untouched.
void apply_hints(schema_t& schema, const heuristic_result_t& hints, bool overwrite_confirmed = false);

std::string format_hints(const heuristic_result_t& hints);

}
