#pragma once

#include "../ai/ai_provider.h"
#include "../dataset/csv_sniffer.h"
#include "../dataset/sampler.h"
#include "../engine/processing_engine.h"
#include "../schema/data_profiler.h"
#include "../schema/schema.h"

#include <string>

namespace merope {

struct inspection_t {
    sniff_result_t     sniff;
    sample_t           sample;
    dataset_profile_t  profile;
    heuristic_result_t heuristics;
    schema_t           schema;
    schema_inference_t ai;
    bool               ai_ran        = false;
    bool               from_cache    = false;
    std::string        cache_path;
};

inspection_t inspect_dataset(const std::string& path, const sample_options_t& sample_options,
                             c_ai_provider* provider, bool use_cache);

// Marks every column confirmed and writes the sidecar. This is what "the user
// pressed confirm in the UI" means at this layer.
bool confirm_and_save(schema_t& schema, std::string& error);

bool overlay_confirmed_schema(schema_t& schema, const std::string& cache_path);

struct query_outcome_t {
    bool                     accepted = false;
    query_plan_t             logical;
    validation_result_t      validation;
    query_result_t           result;
    execution_report_t       report;
};

// Plans, validates and (if accepted) executes. A rejected plan never reaches
// the engine; the reasons come back in validation.errors.
query_outcome_t run_query(const schema_t& schema, c_ai_provider& provider, const std::string& question,
                          const execution_options_t& options);

// Same, for a plan supplied directly as JSON rather than produced by a model.
query_outcome_t run_plan_json(const schema_t& schema, const std::string& plan_json,
                              const execution_options_t& options, std::string& parse_error);

}
