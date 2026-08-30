// parallel/partitioner.h - splits the file into byte ranges for the workers.
//
// Ranges are cut on byte offsets and each reader then snaps forward to the next
// newline, so a record is never split across two partitions (spec 6.2, 15).
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

// Produces at most `desired` partitions over [header_end, file_size).
// Returns a single partition when the file is small, or when the dialect says
// quoted fields contain newlines: cutting on a newline is only safe if every
// newline really ends a record.
partition_plan_t plan_partitions(std::uint64_t file_size, const csv_dialect_t& dialect,
                                 std::size_t desired,
                                 std::uint64_t min_bytes = k_min_partition_bytes);

} // namespace merope
