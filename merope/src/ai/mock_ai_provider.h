#pragma once

#include "ai_provider.h"

namespace merope {

class c_mock_ai_provider final : public c_ai_provider {
public:
    const char* name() const noexcept override;

    schema_inference_t infer_schema(const dataset_profile_t& profile) override;

    query_plan_t generate_query_plan(const schema_t& schema, const std::string& query) override;
};

}
