#include "partitioner.h"

#include <algorithm>

merope::partition_plan_t merope::plan_partitions(std::uint64_t file_size, const csv_dialect_t& dialect,
                                                   std::size_t desired, std::uint64_t min_bytes) {
    partition_plan_t plan;

    if (file_size == 0) {
        plan.note = "empty file";
        return plan;
    }

    if (dialect.quoted_newlines) {
        plan.partitions.push_back(partition_t{0, file_size});
        plan.note = "single partition: quoted fields contain newlines";
        return plan;
    }

    if (desired == 0) desired = 1;
    std::uint64_t count = std::min<std::uint64_t>(desired, std::max<std::uint64_t>(file_size / min_bytes, 1));
    if (count == 0) count = 1;

    if (count == 1) {
        plan.partitions.push_back(partition_t{0, file_size});
        plan.note = file_size < min_bytes ? "single partition: file is smaller than the minimum split"
                                          : "single partition";
        return plan;
    }

    const std::uint64_t stride = file_size / count;
    plan.partitions.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::uint64_t begin = index * stride;
        const std::uint64_t end   = index + 1 == count ? file_size : (index + 1) * stride;
        plan.partitions.push_back(partition_t{begin, end});
    }
    plan.note = std::to_string(count) + " partitions of about " + std::to_string(stride) + " bytes";
    return plan;
}

