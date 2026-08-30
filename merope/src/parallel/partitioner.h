// parallel/partitioner.h - splits the file into byte ranges for the workers.
#pragma once

#include "../dataset/csv_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace merope {

struct partition_t {
    std::uint64_t begin = 0;
    std::uint64_t end   = 0;

    std::uint64_t size() const noexcept { return end - begin; }
};

// Smaller than this and the coordination costs more than the parallelism.
inline constexpr std::uint64_t k_min_partition_bytes = 4ull * 1024 * 1024;

struct partition_plan_t {
    std::vector<partition_t> partitions;
    std::string              note;  // why this many, in the execution report
};

partition_plan_t plan_partitions(std::uint64_t file_size, const csv_dialect_t& dialect,
                                 std::size_t desired,
                                 std::uint64_t min_bytes = k_min_partition_bytes);

}
