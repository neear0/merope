// ai/mock_ai_provider.h - the development and test provider.
//
// It applies the deterministic heuristics, fills in conventional column names
// where the file has no header, and turns a question into a plan by keyword.
// It is deliberately not clever, and says so in its own notes. A production
// adapter implements the same interface against a real model.
#pragma once

#include "ai_provider.h"

namespace merope {

class c_mock_ai_provider final : public c_ai_provider {
public:
    const char* name() const noexcept override;

    schema_inference_t infer_schema(const dataset_profile_t& profile) override;

    query_plan_t generate_query_plan(const schema_t& schema, const std::string& query) override;
};

} // namespace merope
