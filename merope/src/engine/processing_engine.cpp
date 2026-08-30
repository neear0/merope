#include "processing_engine.h"

#include "evaluator.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>

static bool add_checked(std::int64_t& accumulator, std::int64_t value) noexcept {
    if (value > 0 && accumulator > std::numeric_limits<std::int64_t>::max() - value) return false;
    if (value < 0 && accumulator < std::numeric_limits<std::int64_t>::min() - value) return false;
    accumulator += value;
    return true;
}

static void append_raw(std::string& key, const void* bytes, std::size_t size) {
    key.append(static_cast<const char*>(bytes), size);
}

// Encodes group key values into a byte string. Type tags keep 1 (INT64) and
// "1" (text) from colliding into the same group.
static void encode_key_value(std::string& key, const merope::column_block_t& column, std::size_t row) {
    if (column.nulls[row] != 0) {
        key.push_back('\0');
        return;
    }
    switch (merope::storage_kind_of(column.type)) {
    case merope::storage_kind_t::integer: {
        key.push_back('i');
        const std::int64_t value = column.ints[row];
        append_raw(key, &value, sizeof(value));
        break;
    }
    case merope::storage_kind_t::real: {
        key.push_back('d');
        const double value = column.reals[row];
        append_raw(key, &value, sizeof(value));
        break;
    }
    case merope::storage_kind_t::text: {
        key.push_back('s');
        const std::string& value = column.texts[row];
        const std::uint32_t size = static_cast<std::uint32_t>(value.size());
        append_raw(key, &size, sizeof(size));
        key.append(value);
        break;
    }
    case merope::storage_kind_t::none:
        key.push_back('\0');
        break;
    }
}

static merope::cell_value_t cell_from(const merope::column_block_t& column, std::size_t row) {
    return column.value_at(row);
}

static int compare_cells(const merope::cell_value_t& left, const merope::cell_value_t& right) {
    const bool left_null  = merope::is_null(left);
    const bool right_null = merope::is_null(right);
    // Nulls sort last in ascending order, which is what a report reader expects.
    if (left_null || right_null) return left_null == right_null ? 0 : (left_null ? 1 : -1);

    if (const auto* a = std::get_if<std::string>(&left)) {
        const auto* b = std::get_if<std::string>(&right);
        if (b == nullptr) return 0;
        return *a < *b ? -1 : (*a > *b ? 1 : 0);
    }
    if (const auto* a = std::get_if<std::int64_t>(&left)) {
        if (const auto* b = std::get_if<std::int64_t>(&right)) {
            return *a < *b ? -1 : (*a > *b ? 1 : 0);
        }
        if (const auto* b = std::get_if<double>(&right)) {
            const double value = static_cast<double>(*a);
            return value < *b ? -1 : (value > *b ? 1 : 0);
        }
    }
    if (const auto* a = std::get_if<double>(&left)) {
        double b = 0.0;
        if (const auto* real = std::get_if<double>(&right))            b = *real;
        else if (const auto* integral = std::get_if<std::int64_t>(&right)) b = static_cast<double>(*integral);
        else return 0;
        return *a < b ? -1 : (*a > b ? 1 : 0);
    }
    if (const auto* a = std::get_if<bool>(&left)) {
        const auto* b = std::get_if<bool>(&right);
        if (b == nullptr) return 0;
        return *a == *b ? 0 : (*a ? 1 : -1);
    }
    return 0;
}

std::string merope::format_result_table(const query_result_t& result, std::size_t max_rows) {
    std::ostringstream out;
    const std::size_t  column_count = result.columns.size();
    if (column_count == 0) return "  (no columns)\n";

    std::vector<std::string> header = result.columns;
    std::vector<std::size_t> widths(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
        widths[index] = header[index].size();
    }

    const std::size_t shown = std::min(max_rows, result.rows.size());
    std::vector<std::vector<std::string>> cells(shown, std::vector<std::string>(column_count));
    for (std::size_t row = 0; row < shown; ++row) {
        for (std::size_t index = 0; index < column_count; ++index) {
            const data_type_t type = index < result.types.size() ? result.types[index]
                                                                 : data_type_t::utf8;
            cells[row][index] = cell_to_display(result.rows[row][index], type);
            widths[index] = std::max(widths[index], cells[row][index].size());
        }
    }

    auto emit = [&](const std::vector<std::string>& values) {
        out << "  ";
        for (std::size_t index = 0; index < column_count; ++index) {
            std::string padded = values[index];
            if (padded.size() < widths[index]) padded.append(widths[index] - padded.size(), ' ');
            out << padded;
            if (index + 1 < column_count) out << "  ";
        }
        out << "\n";
    };

    emit(header);
    std::size_t rule_width = 0;
    for (const std::size_t width : widths) rule_width += width + 2;
    out << "  " << std::string(rule_width > 2 ? rule_width - 2 : 1, '-') << "\n";
    for (const std::vector<std::string>& row : cells) emit(row);

    if (result.rows.size() > shown) {
        out << "  ... " << (result.rows.size() - shown) << " more row(s)\n";
    }
    if (result.limited) {
        out << "  (result truncated by the plan limit)\n";
    }
    if (!result.warning.empty()) {
        out << "  warning: " << result.warning << "\n";
    }
    return out.str();
}

merope::c_processing_engine::c_processing_engine(const schema_t& schema, const physical_plan_t& plan,
                                         execution_options_t options)
    : m_schema(schema), m_plan(plan), m_options(options) {
    if (m_options.workers == 0)    m_options.workers = default_worker_count();
    if (m_options.partitions == 0) m_options.partitions = m_options.workers;
    if (m_options.chunk_rows == 0) m_options.chunk_rows = k_default_chunk_rows;
}

void merope::c_processing_engine::run_partition(const partition_t& partition, partial_result_t& out) const {
    c_chunk_reader reader(m_schema, m_plan.projection, partition.begin, partition.end,
                          m_options.policy, m_options.chunk_rows);
    c_expression_evaluator evaluator;
    chunk_t                chunk;

    const std::size_t scanned_columns = m_plan.projection.size();
    const bool        aggregating     = m_plan.aggregating();

    std::vector<std::size_t> selected;
    std::string              key;

    while (reader.next_chunk(chunk)) {
        // Computed columns become real columns of the chunk, so everything
        // downstream addresses values uniformly by slot.
        chunk.columns.resize(scanned_columns + m_plan.computed.size());
        for (std::size_t index = 0; index < m_plan.computed.size(); ++index) {
            chunk.columns[scanned_columns + index] =
                evaluator.evaluate_owned(*m_plan.computed[index].expression, chunk);
        }

        selected.clear();
        if (m_plan.filter != nullptr) {
            const value_block_t mask = evaluator.evaluate(*m_plan.filter, chunk);
            for (std::size_t row = 0; row < chunk.row_count; ++row) {
                const std::size_t index = mask.index_for(row);
                // A predicate that evaluates to NULL does not select the row.
                if (mask.block->nulls[index] == 0 && mask.block->ints[index] != 0) {
                    selected.push_back(row);
                }
            }
        } else {
            selected.resize(chunk.row_count);
            for (std::size_t row = 0; row < chunk.row_count; ++row) selected[row] = row;
        }
        out.rows_after_filter += selected.size();

        if (!aggregating) {
            for (const std::size_t row : selected) {
                std::vector<cell_value_t> values;
                values.reserve(m_plan.output_slots.size());
                for (const slot_index_t slot : m_plan.output_slots) {
                    values.push_back(cell_from(chunk.columns[slot], row));
                }
                out.rows.push_back(std::move(values));
            }
            // Without a sort the first `limit` rows are as good as any others,
            // so a partition stops collecting once it has enough.
            if (m_plan.sorts.empty() && m_plan.limit > 0 &&
                out.rows.size() >= static_cast<std::size_t>(m_plan.limit)) {
                out.rows.resize(static_cast<std::size_t>(m_plan.limit));
                break;
            }
            continue;
        }

        for (const std::size_t row : selected) {
            key.clear();
            for (const slot_index_t slot : m_plan.group_by) {
                encode_key_value(key, chunk.columns[slot], row);
            }

            auto found = out.groups.find(key);
            if (found == out.groups.end()) {
                group_state_t state;
                state.aggregates.resize(m_plan.aggregates.size());
                state.key.reserve(m_plan.group_by.size());
                for (const slot_index_t slot : m_plan.group_by) {
                    state.key.push_back(cell_from(chunk.columns[slot], row));
                }
                found = out.groups.emplace(key, std::move(state)).first;
            }

            group_state_t& state = found->second;
            ++state.rows;

            for (std::size_t index = 0; index < m_plan.aggregates.size(); ++index) {
                const aggregate_spec_t& spec        = m_plan.aggregates[index];
                accumulator_t&          accumulator = state.aggregates[index];

                if (spec.counts_rows()) {
                    ++accumulator.count;
                    continue;
                }

                const column_block_t& column = chunk.columns[spec.input_slot];
                if (column.nulls[row] != 0) continue;  // aggregates skip nulls
                ++accumulator.count;

                switch (storage_kind_of(spec.input_type)) {
                case storage_kind_t::integer: {
                    const std::int64_t value = column.ints[row];
                    if (!add_checked(accumulator.int_sum, value)) accumulator.overflow = true;
                    if (!accumulator.has_extreme) {
                        accumulator.int_min = accumulator.int_max = value;
                        accumulator.has_extreme = true;
                    } else {
                        accumulator.int_min = std::min(accumulator.int_min, value);
                        accumulator.int_max = std::max(accumulator.int_max, value);
                    }
                    break;
                }
                case storage_kind_t::real: {
                    const double value = column.reals[row];
                    accumulator.real_sum += value;
                    if (!accumulator.has_extreme) {
                        accumulator.real_min = accumulator.real_max = value;
                        accumulator.has_extreme = true;
                    } else {
                        accumulator.real_min = std::min(accumulator.real_min, value);
                        accumulator.real_max = std::max(accumulator.real_max, value);
                    }
                    break;
                }
                case storage_kind_t::text: {
                    const std::string& value = column.texts[row];
                    if (!accumulator.has_extreme) {
                        accumulator.text_min = accumulator.text_max = value;
                        accumulator.has_extreme = true;
                    } else {
                        if (value < accumulator.text_min) accumulator.text_min = value;
                        if (value > accumulator.text_max) accumulator.text_max = value;
                    }
                    break;
                }
                case storage_kind_t::none:
                    break;
                }
            }
        }
    }

    out.stats      = reader.stats();
    out.quarantine = reader.quarantine();
}

void merope::c_processing_engine::merge(partial_result_t& into, partial_result_t& from) const {
    into.stats.merge(from.stats);
    into.rows_after_filter += from.rows_after_filter;
    for (const std::string& row : from.quarantine) {
        if (into.quarantine.size() < 100) into.quarantine.push_back(row);
    }

    if (!m_plan.aggregating()) {
        into.rows.insert(into.rows.end(), std::make_move_iterator(from.rows.begin()),
                         std::make_move_iterator(from.rows.end()));
        from.rows.clear();
        return;
    }

    for (auto& [key, state] : from.groups) {
        auto found = into.groups.find(key);
        if (found == into.groups.end()) {
            into.groups.emplace(key, std::move(state));
            continue;
        }

        group_state_t& target = found->second;
        target.rows += state.rows;
        for (std::size_t index = 0; index < target.aggregates.size(); ++index) {
            accumulator_t&       left  = target.aggregates[index];
            const accumulator_t& right = state.aggregates[index];
            const data_type_t    type  = m_plan.aggregates[index].input_type;

            left.count += right.count;
            left.real_sum += right.real_sum;
            if (!add_checked(left.int_sum, right.int_sum)) left.overflow = true;
            if (right.overflow) left.overflow = true;

            if (!right.has_extreme) continue;
            if (!left.has_extreme) {
                left.has_extreme = true;
                left.int_min = right.int_min;
                left.int_max = right.int_max;
                left.real_min = right.real_min;
                left.real_max = right.real_max;
                left.text_min = right.text_min;
                left.text_max = right.text_max;
                continue;
            }
            switch (storage_kind_of(type)) {
            case storage_kind_t::integer:
                left.int_min = std::min(left.int_min, right.int_min);
                left.int_max = std::max(left.int_max, right.int_max);
                break;
            case storage_kind_t::real:
                left.real_min = std::min(left.real_min, right.real_min);
                left.real_max = std::max(left.real_max, right.real_max);
                break;
            case storage_kind_t::text:
                if (right.text_min < left.text_min) left.text_min = right.text_min;
                if (right.text_max > left.text_max) left.text_max = right.text_max;
                break;
            case storage_kind_t::none:
                break;
            }
        }
    }
    from.groups.clear();
}

merope::query_result_t merope::c_processing_engine::finalise(partial_result_t& combined) const {
    query_result_t result;
    result.columns = m_plan.result_columns();
    result.types   = m_plan.result_types();

    if (!m_plan.aggregating()) {
        result.rows = std::move(combined.rows);
    } else {
        result.rows.reserve(combined.groups.size());
        bool overflowed = false;

        for (auto& [key, state] : combined.groups) {
            (void)key;
            std::vector<cell_value_t> row = std::move(state.key);
            row.reserve(m_plan.group_by.size() + m_plan.aggregates.size());

            for (std::size_t index = 0; index < m_plan.aggregates.size(); ++index) {
                const aggregate_spec_t& spec        = m_plan.aggregates[index];
                const accumulator_t&    accumulator = state.aggregates[index];
                if (accumulator.overflow) overflowed = true;

                switch (spec.function) {
                case aggregate_function_t::count:
                    row.push_back(cell_value_t{static_cast<std::int64_t>(accumulator.count)});
                    break;

                case aggregate_function_t::sum:
                    if (accumulator.count == 0) {
                        row.push_back(cell_value_t{});
                    } else if (storage_kind_of(spec.input_type) == storage_kind_t::real) {
                        row.push_back(cell_value_t{accumulator.real_sum});
                    } else {
                        row.push_back(cell_value_t{accumulator.int_sum});
                    }
                    break;

                case aggregate_function_t::average: {
                    // The partial state is (sum, count); the mean is formed
                    // once, here, never by averaging partial means.
                    if (accumulator.count == 0) {
                        row.push_back(cell_value_t{});
                        break;
                    }
                    const double divisor = static_cast<double>(accumulator.count);
                    double       mean    = 0.0;
                    if (storage_kind_of(spec.input_type) == storage_kind_t::real) {
                        mean = accumulator.real_sum / divisor;
                    } else if (spec.input_type == data_type_t::decimal) {
                        mean = static_cast<double>(accumulator.int_sum) / divisor /
                               static_cast<double>(k_money_factor);
                    } else {
                        mean = static_cast<double>(accumulator.int_sum) / divisor;
                    }
                    row.push_back(cell_value_t{mean});
                    break;
                }

                case aggregate_function_t::minimum:
                case aggregate_function_t::maximum: {
                    const bool want_min = spec.function == aggregate_function_t::minimum;
                    if (!accumulator.has_extreme) {
                        row.push_back(cell_value_t{});
                        break;
                    }
                    switch (storage_kind_of(spec.input_type)) {
                    case storage_kind_t::integer:
                        row.push_back(cell_value_t{want_min ? accumulator.int_min : accumulator.int_max});
                        break;
                    case storage_kind_t::real:
                        row.push_back(cell_value_t{want_min ? accumulator.real_min : accumulator.real_max});
                        break;
                    case storage_kind_t::text:
                        row.push_back(cell_value_t{want_min ? accumulator.text_min : accumulator.text_max});
                        break;
                    case storage_kind_t::none:
                        row.push_back(cell_value_t{});
                        break;
                    }
                    break;
                }
                }
            }
            result.rows.push_back(std::move(row));
        }

        if (overflowed) {
            result.warning = "an INT64 sum exceeded the 64 bit range; that value is not reliable";
        }
    }

    if (!m_plan.sorts.empty()) {
        const std::vector<sort_spec_t>& sorts = m_plan.sorts;
        std::stable_sort(result.rows.begin(), result.rows.end(),
                         [&sorts](const std::vector<cell_value_t>& left,
                                  const std::vector<cell_value_t>& right) {
                             for (const sort_spec_t& spec : sorts) {
                                 if (spec.output_index >= left.size()) continue;
                                 const int ordering =
                                     compare_cells(left[spec.output_index], right[spec.output_index]);
                                 if (ordering == 0) continue;
                                 return spec.order == sort_order_t::descending ? ordering > 0
                                                                               : ordering < 0;
                             }
                             return false;
                         });
    }

    if (m_plan.limit > 0 && result.rows.size() > static_cast<std::size_t>(m_plan.limit)) {
        result.rows.resize(static_cast<std::size_t>(m_plan.limit));
        result.limited = true;
    }

    return result;
}

merope::query_result_t merope::c_processing_engine::run() {
    using steady_clock_t = std::chrono::steady_clock;

    if (m_plan.aggregates.empty() && m_plan.output_slots.empty()) {
        throw std::runtime_error("refusing to execute a plan that was not accepted by the validator");
    }

    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(m_schema.dataset_path, ec);
    if (ec) throw std::runtime_error("cannot stat dataset: " + m_schema.dataset_path);

    m_report = execution_report_t{};
    m_report.dataset_size_bytes = file_size;
    m_report.bad_row_policy     = to_string(m_options.policy);

    // Optional single threaded baseline, measured on the same work so the
    // speedup in the report is a fact rather than an estimate.
    if (m_options.measure_baseline) {
        partition_plan_t single = plan_partitions(file_size, m_schema.dialect, 1, m_options.min_partition_bytes);
        partial_result_t scratch;
        const auto       started = steady_clock_t::now();
        for (const partition_t& partition : single.partitions) {
            partial_result_t local;
            run_partition(partition, local);
            merge(scratch, local);
        }
        (void)finalise(scratch);
        m_report.has_baseline     = true;
        m_report.baseline_seconds =
            std::chrono::duration<double>(steady_clock_t::now() - started).count();
    }

    const partition_plan_t partitions =
        plan_partitions(file_size, m_schema.dialect, m_options.partitions, m_options.min_partition_bytes);
    m_report.partitions     = partitions.partitions.size();
    m_report.partition_note = partitions.note;
    m_report.workers        = std::min(m_options.workers, std::max<std::size_t>(m_report.partitions, 1));

    std::vector<partial_result_t> locals(partitions.partitions.size());
    const auto                    started = steady_clock_t::now();

    if (m_report.workers <= 1 || partitions.partitions.size() <= 1) {
        for (std::size_t index = 0; index < partitions.partitions.size(); ++index) {
            run_partition(partitions.partitions[index], locals[index]);
        }
    } else {
        c_thread_pool pool(m_report.workers);
        for (std::size_t index = 0; index < partitions.partitions.size(); ++index) {
            pool.submit([this, &partitions, &locals, index] {
                run_partition(partitions.partitions[index], locals[index]);
            });
        }
        pool.wait_for_all();
    }

    // Global reduce.
    partial_result_t combined;
    for (partial_result_t& local : locals) merge(combined, local);
    query_result_t result = finalise(combined);

    m_report.processing_seconds = std::chrono::duration<double>(steady_clock_t::now() - started).count();
    m_report.records_processed  = combined.stats.rows_read;
    m_report.bad_rows           = combined.stats.bad_rows;
    m_report.quarantined_rows   = combined.stats.quarantined;
    m_report.bytes_scanned      = combined.stats.bytes_consumed;
    m_report.rows_after_filter  = combined.rows_after_filter;
    m_report.groups             = m_plan.aggregating() ? result.rows.size() : 0;
    m_report.peak_memory        = peak_memory_bytes();

    return result;
}

