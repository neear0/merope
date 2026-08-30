// plan/expression_parser.h - the lexer and recursive descent parser behind
// parse_expression().
//
// Implementation detail of expression.cpp; it has a header only because a class
// declaration belongs in one. Callers use parse_expression().
#pragma once

#include "expression.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace merope {

enum class token_kind_t : std::uint8_t { end, identifier, number, string, punctuation, keyword };

struct token_t {
    token_kind_t kind     = token_kind_t::end;
    std::string  text;
    double       number   = 0.0;
    bool         integral = false;
    std::size_t  offset   = 0;
};

class c_lexer {
public:
    explicit c_lexer(std::string_view text);

    bool tokenize(std::vector<token_t>& out, std::string& error);

private:
    void skip_space();

    std::string_view m_text;
    std::size_t      m_cursor = 0;
};

class c_expr_parser {
public:
    c_expr_parser(std::vector<token_t> tokens, std::string& error);

    expr_ptr run();

private:
    // The grammar is recursive descent, so nesting depth is stack depth and a
    // few thousand '(' would overflow it. A stack overflow is not a catchable
    // std::exception, so the API's try/catch could not turn it into a 500 --
    // the process would simply die. The JSON parser guards itself the same way.
    static constexpr std::size_t k_max_depth = 64;

    // Taken in the three functions every unbounded recursion path goes through:
    // parse_or (re-entered by a parenthesised group) and the two that recurse
    // into themselves directly.
    class c_depth_guard {
    public:
        explicit c_depth_guard(std::size_t& depth) noexcept : m_depth(depth) { ++m_depth; }
        ~c_depth_guard() { --m_depth; }
        c_depth_guard(const c_depth_guard&)            = delete;
        c_depth_guard& operator=(const c_depth_guard&) = delete;

    private:
        std::size_t& m_depth;
    };

    const token_t& peek(std::size_t ahead = 0) const;
    const token_t& advance();
    bool           accept_punctuation(std::string_view text);
    bool           accept_keyword(std::string_view word);
    std::nullptr_t fail(const std::string& message);

    expr_ptr parse_or();
    expr_ptr parse_and();
    expr_ptr parse_not();
    expr_ptr parse_comparison();
    expr_ptr parse_additive();
    expr_ptr parse_multiplicative();
    expr_ptr parse_unary();
    expr_ptr parse_primary();

    std::vector<token_t> m_tokens;
    std::string&         m_error;
    std::size_t          m_cursor = 0;
    std::size_t          m_depth  = 0;
};

} // namespace merope
