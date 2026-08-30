// engine/processing_engine.h - executes a validated physical plan (spec 6).
//
// Each worker owns one byte range, streams it chunk by chunk, and reduces it
// into a local partial result. The coordinator merges those partials into the
// global result. Nothing accumulates per input row outside a group.
#pragma once

#include "../dataset/csv_reader.h"
#include "../parallel/partitioner.h"
#include "../parallel/thread_pool.h"
#include "../plan/plan_validator.h"
#include "../schema/schema.h"
#include "execution_report.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace merope {

struct query_result_t {
    std::vector<std::string>               columns;
    std::vector<data_type_t>               types;
    std::vector<std::vector<cell_value_t>> rows;

    bool        limited = false;  // rows were cut off by the plan limit
    std::string warning;          // e.g. an integer sum that would have overflowed
};

std::string format_result_table(const query_result_t& result, std::size_t max_rows = 50);

struct execution_options_t {
    std::size_t      workers      = 0;  // 0 means one per hardware thread
    std::size_t      partitions   = 0;  // 0 means one per worker
    std::size_t      chunk_rows   = k_default_chunk_rows;
    bad_row_policy_t policy       = bad_row_policy_t::skip;

    // Smallest partition worth creating. Lowering it forces splitting on small
    // files, which is what the scaling experiments need.
    std::uint64_t min_partition_bytes = k_min_partition_bytes;

    // Runs the whole query single threaded first, so the report can state a
    // measured speedup instead of an assumed one.
    bool measure_baseline = false;
};

class c_processing_engine {
public:
    c_processing_engine(const schema_t& schema, const physical_plan_t& plan,
                        execution_options_t options = {});

    // Runs the plan. Throws std::runtime_error on an unrecoverable failure
    // (a bad row under the `fail` policy, an unreadable file).
    query_result_t run();

    const execution_report_t& report() const noexcept { return m_report; }

private:
    // What one partition produces. Group keys are encoded into a string so the
    // hash map does not have to hash a vector of variants on every row.
    struct accumulator_t {
        std::uint64_t count     = 0;
        std::int64_t  int_sum   = 0;
        double        real_sum  = 0.0;
        bool          overflow  = false;
        bool          has_extreme = false;
        std::int64_t  int_min = 0;
        std::int64_t  int_max = 0;
        double        real_min = 0.0;
        double        real_max = 0.0;
        std::string   text_min;
        std::string   text_max;
    };

    struct group_state_t {
        std::vector<cell_value_t>  key;
        std::vector<accumulator_t> aggregates;
        std::uint64_t              rows = 0;
    };

    struct partial_result_t {
        std::unordered_map<std::string, group_state_t> groups;
        std::vector<std::vector<cell_value_t>>         rows;   // when not aggregating
        read_stats_t                                   stats;
        std::uint64_t                                  rows_after_filter = 0;
        std::vector<std::string>                       quarantine;
    };

    void run_partition(const partition_t& partition, partial_result_t& out) const;
    void merge(partial_result_t& into, partial_result_t& from) const;
    query_result_t finalise(partial_result_t& combined) const;

    const schema_t&        m_schema;
    const physical_plan_t& m_plan;
    execution_options_t    m_options;
    execution_report_t     m_report;
};

} // namespace merope
