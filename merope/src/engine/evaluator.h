#pragma once

#include "../plan/expression.h"
#include "chunk.h"

#include <memory>
#include <vector>

namespace merope {

// A value produced by one expression node: either a full column of `row_count`
// values, or a single scalar (a literal) that applies to every row.
struct value_block_t {
    const column_block_t* block     = nullptr;
    bool                  is_scalar = false;

    std::size_t index_for(std::size_t row) const noexcept { return is_scalar ? 0 : row; }
    data_type_t type() const noexcept { return block->type; }
};

// Reuses its scratch blocks between chunks, so steady state evaluation does no
// allocation beyond what string values require.
class c_expression_evaluator {
public:
    // The returned block is valid until the next call to evaluate().
    value_block_t evaluate(const expr_t& expr, const chunk_t& chunk);

    // Materialises the result as an owned block, for computed columns that get
    // appended to the chunk and referenced by later operations.
    column_block_t evaluate_owned(const expr_t& expr, const chunk_t& chunk);

private:
    value_block_t   eval_node(const expr_t& expr, const chunk_t& chunk);
    column_block_t& acquire(data_type_t type, std::size_t rows);

    std::vector<std::unique_ptr<column_block_t>> m_pool;
    std::size_t                                  m_used = 0;
};

}
