#include "pipeline.h"

#include "../schema/heuristic_inference.h"

merope::inspection_t merope::inspect_dataset(const std::string& path, const sample_options_t& sample_options,
                                               c_ai_provider* provider, bool use_cache) {
    inspection_t inspection;
    inspection.cache_path = schema_sidecar_path(path);

    if (use_cache) {
        std::string error;
        if (load_schema(inspection.cache_path, inspection.schema, error)) {
            // A confirmed schema is the answer; re-profiling would only risk
            // contradicting a decision the user already made.
            inspection.schema.dataset_path = path;
            inspection.from_cache          = true;
            return inspection;
        }
    }

    inspection.sniff   = sniff_csv(path);
    inspection.sample  = sample_dataset(path, inspection.sniff.dialect, sample_options);
    inspection.profile = profile_sample(path, inspection.sniff.dialect, inspection.sample);
    inspection.heuristics = infer_heuristically(inspection.profile);
    inspection.schema     = build_schema(inspection.profile, inspection.heuristics);

    if (provider != nullptr) {
        inspection.ai     = provider->infer_schema(inspection.profile);
        inspection.ai_ran = true;
        // The AI proposal replaces the heuristic one, but only where the user
        // has not already decided.
        apply_hints(inspection.schema, inspection.ai.proposals);
    }

    return inspection;
}

bool merope::confirm_and_save(schema_t& schema, std::string& error) {
    for (column_schema_t& column : schema.columns) {
        column.user_confirmed = true;
    }
    return save_schema(schema, schema_sidecar_path(schema.dataset_path), error);
}

bool merope::overlay_confirmed_schema(schema_t& schema, const std::string& cache_path) {
    schema_t    cached;
    std::string error;
    if (!load_schema(cache_path, cached, error)) return false;

    for (const column_schema_t& saved : cached.columns) {
        if (saved.physical_index >= schema.columns.size()) continue;
        column_schema_t& column = schema.columns[saved.physical_index];
        // A cached decision only applies if it is about the same column. If the
        // file changed shape underneath us, the fresh profile wins.
        if (!saved.physical_name.empty() && saved.physical_name != column.physical_name) continue;
        column.semantic_name  = saved.semantic_name;
        column.semantic_type  = saved.semantic_type;
        column.confidence     = saved.confidence;
        column.user_confirmed = saved.user_confirmed;
    }
    return true;
}

// Both query entry points funnel through here, so a plan can only ever reach
// the engine after the validator has accepted it.
static merope::query_outcome_t execute_validated(const merope::schema_t& schema,
                                                  merope::query_plan_t logical,
                                                  const merope::execution_options_t& options) {
    merope::query_outcome_t outcome;
    outcome.logical    = std::move(logical);
    outcome.validation = merope::validate_plan(outcome.logical, schema);
    if (!outcome.validation.accepted) return outcome;

    merope::c_processing_engine engine(schema, outcome.validation.plan, options);
    outcome.result   = engine.run();
    outcome.report   = engine.report();
    outcome.accepted = true;
    return outcome;
}

merope::query_outcome_t merope::run_query(const schema_t& schema, c_ai_provider& provider, const std::string& question,
                                            const execution_options_t& options) {
    return execute_validated(schema, provider.generate_query_plan(schema, question), options);
}

merope::query_outcome_t merope::run_plan_json(const schema_t& schema, const std::string& plan_json,
                                                const execution_options_t& options, std::string& parse_error) {
    query_plan_t logical;
    if (!parse_query_plan(plan_json, logical, parse_error)) {
        return query_outcome_t{};
    }
    return execute_validated(schema, std::move(logical), options);
}

