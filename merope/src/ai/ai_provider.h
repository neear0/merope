// ai/ai_provider.h - the only place the engine talks to a model (spec 8.1).
//
// Two hard rules hold on both sides of this interface:
//   - the model never sees the dataset, only a profile and sample values;
//   - the model never produces code, only a declarative plan that the
//     validator then has to accept before anything executes.
#pragma once

#include "../plan/query_plan.h"
#include "../schema/data_profiler.h"
#include "../schema/heuristic_inference.h"
#include "../schema/schema.h"

#include <memory>
#include <string>
#include <vector>

namespace merope {

struct schema_inference_t {
    std::string        provider;
    heuristic_result_t proposals;   // one proposal per physical column
    std::vector<std::string> notes; // what the provider did, and what it refused to guess

    // True when the provider left at least one column as UNKNOWN, which is an
    // expected and legal outcome, not a failure.
    bool has_unknown_columns() const noexcept;
};

class c_ai_provider {
public:
    virtual schema_inference_t infer_schema(
        const dataset_profile_t& profile) = 0;

    virtual query_plan_t generate_query_plan(
        const schema_t& schema,
        const std::string& query) = 0;

    virtual const char* name() const noexcept = 0;

    virtual ~c_ai_provider() = default;
};

// Builds the prompt payload a real adapter would send: the profile and a few
// representative values per column, never the dataset itself. Exposed here so
// that what leaves the process is inspectable and testable.
std::string build_schema_prompt(const dataset_profile_t& profile);
std::string build_plan_prompt(const schema_t& schema, const std::string& query);

// The development and test provider. A production adapter implements the same
// interface against a real model and lives outside the engine.
std::unique_ptr<c_ai_provider> make_mock_provider();

} // namespace merope
