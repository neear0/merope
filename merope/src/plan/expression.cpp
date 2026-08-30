#include "expression.h"

#include "../core/parse.h"
#include "expression_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>

static bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

static bool is_keyword(std::string_view word) {
    static constexpr std::string_view k_keywords[] = {
        "and", "or", "not", "in", "between", "is", "null", "true", "false"
    };
    return std::any_of(std::begin(k_keywords), std::end(k_keywords),
                       [&](std::string_view keyword) { return equals_ignore_case(word, keyword); });
}

// --------------------------------------------------------------- lexer -----

merope::c_lexer::c_lexer(std::string_view text) : m_text(text) {}

void merope::c_lexer::skip_space() {
    while (m_cursor < m_text.size() &&
           std::isspace(static_cast<unsigned char>(m_text[m_cursor])) != 0) {
        ++m_cursor;
    }
}

bool merope::c_lexer::tokenize(std::vector<token_t>& out, std::string& error) {
    while (true) {
        skip_space();
        if (m_cursor >= m_text.size()) {
            out.push_back(token_t{token_kind_t::end, "", 0.0, false, m_cursor});
            return true;
        }

        const std::size_t start = m_cursor;
        const char        ch    = m_text[m_cursor];

        if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
            while (m_cursor < m_text.size() &&
                   (std::isalnum(static_cast<unsigned char>(m_text[m_cursor])) != 0 ||
                    m_text[m_cursor] == '_')) {
                ++m_cursor;
            }
            std::string word(m_text.substr(start, m_cursor - start));
            const token_kind_t kind = is_keyword(word) ? token_kind_t::keyword : token_kind_t::identifier;
            out.push_back(token_t{kind, std::move(word), 0.0, false, start});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            bool integral = true;
            while (m_cursor < m_text.size() &&
                   std::isdigit(static_cast<unsigned char>(m_text[m_cursor])) != 0) {
                ++m_cursor;
            }
            if (m_cursor < m_text.size() && m_text[m_cursor] == '.') {
                integral = false;
                ++m_cursor;
                while (m_cursor < m_text.size() &&
                       std::isdigit(static_cast<unsigned char>(m_text[m_cursor])) != 0) {
                    ++m_cursor;
                }
            }
            const std::string digits(m_text.substr(start, m_cursor - start));
            out.push_back(token_t{token_kind_t::number, digits,
                                  std::strtod(digits.c_str(), nullptr), integral, start});
            continue;
        }

        if (ch == '\'' || ch == '"') {
            const char quote = ch;
            ++m_cursor;
            std::string value;
            while (m_cursor < m_text.size() && m_text[m_cursor] != quote) {
                if (m_text[m_cursor] == '\\' && m_cursor + 1 < m_text.size()) ++m_cursor;
                value.push_back(m_text[m_cursor]);
                ++m_cursor;
            }
            if (m_cursor >= m_text.size()) {
                error = "unterminated string literal at offset " + std::to_string(start);
                return false;
            }
            ++m_cursor;  // closing quote
            out.push_back(token_t{token_kind_t::string, std::move(value), 0.0, false, start});
            continue;
        }

        // Two character operators first, so != does not lex as ! then =.
        static constexpr std::string_view k_double[] = {"==", "!=", "<=", ">=", "<>"};
        bool matched = false;
        for (const std::string_view candidate : k_double) {
            if (m_text.compare(m_cursor, 2, candidate) == 0) {
                m_cursor += 2;
                out.push_back(token_t{token_kind_t::punctuation, std::string(candidate), 0.0, false, start});
                matched = true;
                break;
            }
        }
        if (matched) continue;

        static constexpr std::string_view k_single = "+-*/<>=(),";
        if (k_single.find(ch) != std::string_view::npos) {
            ++m_cursor;
            out.push_back(token_t{token_kind_t::punctuation, std::string(1, ch), 0.0, false, start});
            continue;
        }

        error = std::string("unexpected character '") + ch + "' at offset " + std::to_string(start);
        return false;
    }
}

// -------------------------------------------------------------- parser -----

merope::c_expr_parser::c_expr_parser(std::vector<token_t> tokens, std::string& error)
    : m_tokens(std::move(tokens)), m_error(error) {}

const merope::token_t& merope::c_expr_parser::peek(std::size_t ahead) const {
    const std::size_t index = std::min(m_cursor + ahead, m_tokens.size() - 1);
    return m_tokens[index];
}

const merope::token_t& merope::c_expr_parser::advance() {
    const token_t& token = m_tokens[m_cursor];
    if (m_cursor + 1 < m_tokens.size()) ++m_cursor;
    return token;
}

bool merope::c_expr_parser::accept_punctuation(std::string_view text) {
    if (peek().kind == token_kind_t::punctuation && peek().text == text) {
        advance();
        return true;
    }
    return false;
}

bool merope::c_expr_parser::accept_keyword(std::string_view word) {
    if (peek().kind == token_kind_t::keyword && equals_ignore_case(peek().text, word)) {
        advance();
        return true;
    }
    return false;
}

std::nullptr_t merope::c_expr_parser::fail(const std::string& message) {
    if (m_error.empty()) {
        m_error = message + " (at offset " + std::to_string(peek().offset) + ")";
    }
    return nullptr;
}

merope::expr_ptr merope::c_expr_parser::run() {
    expr_ptr root = parse_or();
    if (root == nullptr) return nullptr;
    if (peek().kind != token_kind_t::end) {
        fail("unexpected trailing token '" + peek().text + "'");
        return nullptr;
    }
    return root;
}

merope::expr_ptr merope::c_expr_parser::parse_or() {
    const c_depth_guard guard(m_depth);
    if (m_depth > k_max_depth) return fail("expression nests too deeply");
    expr_ptr left = parse_and();
    if (left == nullptr) return nullptr;
    while (accept_keyword("or")) {
        expr_ptr right = parse_and();
        if (right == nullptr) return nullptr;
        left = make_binary(binary_op_t::logical_or, std::move(left), std::move(right));
    }
    return left;
}

merope::expr_ptr merope::c_expr_parser::parse_and() {
    expr_ptr left = parse_not();
    if (left == nullptr) return nullptr;
    while (accept_keyword("and")) {
        expr_ptr right = parse_not();
        if (right == nullptr) return nullptr;
        left = make_binary(binary_op_t::logical_and, std::move(left), std::move(right));
    }
    return left;
}

merope::expr_ptr merope::c_expr_parser::parse_not() {
    const c_depth_guard guard(m_depth);
    if (m_depth > k_max_depth) return fail("expression nests too deeply");
    if (accept_keyword("not")) {
        expr_ptr operand = parse_not();
        if (operand == nullptr) return nullptr;
        auto node = std::make_unique<expr_t>();
        node->kind = expr_kind_t::logical_not;
        node->args.push_back(std::move(operand));
        return node;
    }
    return parse_comparison();
}

merope::expr_ptr merope::c_expr_parser::parse_comparison() {
    expr_ptr left = parse_additive();
    if (left == nullptr) return nullptr;

    // IS [NOT] NULL
    if (accept_keyword("is")) {
        const bool negated = accept_keyword("not");
        if (!accept_keyword("null")) return fail("expected NULL after IS");
        auto node = std::make_unique<expr_t>();
        node->kind    = expr_kind_t::is_null;
        node->negated = negated;
        node->args.push_back(std::move(left));
        return node;
    }

    bool negated_membership = false;
    if (peek().kind == token_kind_t::keyword && equals_ignore_case(peek().text, "not") &&
        peek(1).kind == token_kind_t::keyword &&
        (equals_ignore_case(peek(1).text, "in") || equals_ignore_case(peek(1).text, "between"))) {
        advance();
        negated_membership = true;
    }

    if (accept_keyword("in")) {
        if (!accept_punctuation("(")) return fail("expected ( after IN");
        auto node = std::make_unique<expr_t>();
        node->kind    = expr_kind_t::in_list;
        node->negated = negated_membership;
        node->args.push_back(std::move(left));
        if (!accept_punctuation(")")) {
            while (true) {
                expr_ptr item = parse_additive();
                if (item == nullptr) return nullptr;
                node->args.push_back(std::move(item));
                if (accept_punctuation(",")) continue;
                if (accept_punctuation(")")) break;
                return fail("expected , or ) in IN list");
            }
        }
        if (node->args.size() < 2) return fail("IN list is empty");
        return node;
    }

    if (accept_keyword("between")) {
        expr_ptr low = parse_additive();
        if (low == nullptr) return nullptr;
        if (!accept_keyword("and")) return fail("expected AND in BETWEEN");
        expr_ptr high = parse_additive();
        if (high == nullptr) return nullptr;
        auto node = std::make_unique<expr_t>();
        node->kind    = expr_kind_t::between;
        node->negated = negated_membership;
        node->args.push_back(std::move(left));
        node->args.push_back(std::move(low));
        node->args.push_back(std::move(high));
        return node;
    }

    if (negated_membership) return fail("expected IN or BETWEEN after NOT");

    if (peek().kind == token_kind_t::punctuation) {
        binary_op_t op = binary_op_t::equal;
        bool        found = true;
        const std::string& text = peek().text;
        if (text == "==" || text == "=")       op = binary_op_t::equal;
        else if (text == "!=" || text == "<>") op = binary_op_t::not_equal;
        else if (text == "<")                  op = binary_op_t::less;
        else if (text == "<=")                 op = binary_op_t::less_equal;
        else if (text == ">")                  op = binary_op_t::greater;
        else if (text == ">=")                 op = binary_op_t::greater_equal;
        else                                   found = false;

        if (found) {
            advance();
            expr_ptr right = parse_additive();
            if (right == nullptr) return nullptr;
            return make_binary(op, std::move(left), std::move(right));
        }
    }

    return left;
}

merope::expr_ptr merope::c_expr_parser::parse_additive() {
    expr_ptr left = parse_multiplicative();
    if (left == nullptr) return nullptr;
    while (peek().kind == token_kind_t::punctuation &&
           (peek().text == "+" || peek().text == "-")) {
        const binary_op_t op = peek().text == "+" ? binary_op_t::add : binary_op_t::subtract;
        advance();
        expr_ptr right = parse_multiplicative();
        if (right == nullptr) return nullptr;
        left = make_binary(op, std::move(left), std::move(right));
    }
    return left;
}

merope::expr_ptr merope::c_expr_parser::parse_multiplicative() {
    expr_ptr left = parse_unary();
    if (left == nullptr) return nullptr;
    while (peek().kind == token_kind_t::punctuation &&
           (peek().text == "*" || peek().text == "/")) {
        const binary_op_t op = peek().text == "*" ? binary_op_t::multiply : binary_op_t::divide;
        advance();
        expr_ptr right = parse_unary();
        if (right == nullptr) return nullptr;
        left = make_binary(op, std::move(left), std::move(right));
    }
    return left;
}

merope::expr_ptr merope::c_expr_parser::parse_unary() {
    const c_depth_guard guard(m_depth);
    if (m_depth > k_max_depth) return fail("expression nests too deeply");
    if (peek().kind == token_kind_t::punctuation && peek().text == "-") {
        advance();
        expr_ptr operand = parse_unary();
        if (operand == nullptr) return nullptr;
        auto node = std::make_unique<expr_t>();
        node->kind = expr_kind_t::unary_negate;
        node->args.push_back(std::move(operand));
        return node;
    }
    if (peek().kind == token_kind_t::punctuation && peek().text == "+") {
        advance();
        return parse_unary();
    }
    return parse_primary();
}

merope::expr_ptr merope::c_expr_parser::parse_primary() {
    const token_t& token = peek();

    if (token.kind == token_kind_t::number) {
        advance();
        if (token.integral) {
            return make_literal(cell_value_t{static_cast<std::int64_t>(token.number)},
                                data_type_t::int64);
        }
        return make_literal(cell_value_t{token.number}, data_type_t::float64);
    }

    if (token.kind == token_kind_t::string) {
        advance();
        return make_literal(cell_value_t{token.text}, data_type_t::utf8);
    }

    if (token.kind == token_kind_t::keyword) {
        if (accept_keyword("true"))  return make_literal(cell_value_t{true}, data_type_t::boolean);
        if (accept_keyword("false")) return make_literal(cell_value_t{false}, data_type_t::boolean);
        if (accept_keyword("null"))  return make_literal(cell_value_t{}, data_type_t::unknown);
        return fail("unexpected keyword '" + token.text + "'");
    }

    if (token.kind == token_kind_t::punctuation && token.text == "(") {
        advance();
        expr_ptr inner = parse_or();
        if (inner == nullptr) return nullptr;
        if (!accept_punctuation(")")) return fail("expected )");
        return inner;
    }

    if (token.kind == token_kind_t::identifier) {
        advance();
        if (peek().kind == token_kind_t::punctuation && peek().text == "(") {
            function_t function = function_t::year;
            if (!function_from_string(token.text, function)) {
                return fail("unknown function '" + token.text + "'");
            }
            advance();  // '('
            auto node = std::make_unique<expr_t>();
            node->kind     = expr_kind_t::function;
            node->function = function;
            node->name     = token.text;
            if (!accept_punctuation(")")) {
                while (true) {
                    expr_ptr argument = parse_or();
                    if (argument == nullptr) return nullptr;
                    node->args.push_back(std::move(argument));
                    if (accept_punctuation(",")) continue;
                    if (accept_punctuation(")")) break;
                    return fail("expected , or ) in argument list");
                }
            }
            if (node->args.size() != 1) {
                return fail("function '" + token.text + "' takes exactly one argument");
            }
            return node;
        }
        return make_column(token.text);
    }

    return fail("unexpected end of expression");
}

// ---------------------------------------------------------- public API -----

const char* merope::to_string(binary_op_t op) noexcept {
    switch (op) {
    case binary_op_t::add:           return "+";
    case binary_op_t::subtract:      return "-";
    case binary_op_t::multiply:      return "*";
    case binary_op_t::divide:        return "/";
    case binary_op_t::equal:         return "==";
    case binary_op_t::not_equal:     return "!=";
    case binary_op_t::less:          return "<";
    case binary_op_t::less_equal:    return "<=";
    case binary_op_t::greater:       return ">";
    case binary_op_t::greater_equal: return ">=";
    case binary_op_t::logical_and:   return "AND";
    case binary_op_t::logical_or:    return "OR";
    default:                         return "?";
    }
}

const char* merope::to_string(function_t function) noexcept {
    switch (function) {
    case function_t::year:      return "year";
    case function_t::month:     return "month";
    case function_t::day:       return "day";
    case function_t::hour:      return "hour";
    case function_t::minute:    return "minute";
    case function_t::lower:     return "lower";
    case function_t::upper:     return "upper";
    case function_t::length:    return "length";
    case function_t::abs_value: return "abs";
    default:                    return "?";
    }
}

bool merope::function_from_string(std::string_view name, function_t& out) noexcept {
    if (equals_ignore_case(name, "year"))   { out = function_t::year;      return true; }
    if (equals_ignore_case(name, "month"))  { out = function_t::month;     return true; }
    if (equals_ignore_case(name, "day"))    { out = function_t::day;       return true; }
    if (equals_ignore_case(name, "hour"))   { out = function_t::hour;      return true; }
    if (equals_ignore_case(name, "minute")) { out = function_t::minute;    return true; }
    if (equals_ignore_case(name, "lower"))  { out = function_t::lower;     return true; }
    if (equals_ignore_case(name, "upper"))  { out = function_t::upper;     return true; }
    if (equals_ignore_case(name, "length")) { out = function_t::length;    return true; }
    if (equals_ignore_case(name, "abs"))    { out = function_t::abs_value; return true; }
    return false;
}

bool merope::is_comparison(binary_op_t op) noexcept {
    switch (op) {
    case binary_op_t::equal:
    case binary_op_t::not_equal:
    case binary_op_t::less:
    case binary_op_t::less_equal:
    case binary_op_t::greater:
    case binary_op_t::greater_equal:
        return true;
    default:
        return false;
    }
}

bool merope::is_arithmetic(binary_op_t op) noexcept {
    switch (op) {
    case binary_op_t::add:
    case binary_op_t::subtract:
    case binary_op_t::multiply:
    case binary_op_t::divide:
        return true;
    default:
        return false;
    }
}

bool merope::is_logical(binary_op_t op) noexcept {
    return op == binary_op_t::logical_and || op == binary_op_t::logical_or;
}

merope::expr_ptr merope::make_column(std::string name) {
    auto node = std::make_unique<expr_t>();
    node->kind = expr_kind_t::column;
    node->name = std::move(name);
    return node;
}

merope::expr_ptr merope::make_literal(cell_value_t value, data_type_t type) {
    auto node = std::make_unique<expr_t>();
    node->kind         = expr_kind_t::literal;
    node->literal      = std::move(value);
    node->literal_type = type;
    node->result_type  = type;
    return node;
}

merope::expr_ptr merope::make_binary(binary_op_t op, expr_ptr left, expr_ptr right) {
    auto node = std::make_unique<expr_t>();
    node->kind = expr_kind_t::binary;
    node->op   = op;
    node->args.push_back(std::move(left));
    node->args.push_back(std::move(right));
    return node;
}

merope::expr_ptr merope::parse_expression(std::string_view text, std::string& error) {
    error.clear();
    if (trim(text).empty()) {
        error = "expression is empty";
        return nullptr;
    }

    std::vector<token_t> tokens;
    c_lexer lexer(text);
    if (!lexer.tokenize(tokens, error)) return nullptr;

    c_expr_parser parser(std::move(tokens), error);
    expr_ptr root = parser.run();
    if (root == nullptr && error.empty()) error = "could not parse the expression";
    return root;
}

std::string merope::expression_to_string(const expr_t& expr) {
    switch (expr.kind) {
    case expr_kind_t::column:
        return expr.name;
    case expr_kind_t::literal:
        if (is_null(expr.literal)) return "NULL";
        if (expr.literal_type == data_type_t::utf8 || expr.literal_type == data_type_t::categorical) {
            return "'" + cell_to_display(expr.literal, expr.literal_type) + "'";
        }
        return cell_to_display(expr.literal, expr.literal_type);
    case expr_kind_t::function:
        return std::string(to_string(expr.function)) + "(" + expression_to_string(*expr.args[0]) + ")";
    case expr_kind_t::binary:
        return "(" + expression_to_string(*expr.args[0]) + " " + to_string(expr.op) + " " +
               expression_to_string(*expr.args[1]) + ")";
    case expr_kind_t::unary_negate:
        return "-" + expression_to_string(*expr.args[0]);
    case expr_kind_t::logical_not:
        return "NOT " + expression_to_string(*expr.args[0]);
    case expr_kind_t::is_null:
        return expression_to_string(*expr.args[0]) + (expr.negated ? " IS NOT NULL" : " IS NULL");
    case expr_kind_t::between:
        return expression_to_string(*expr.args[0]) + (expr.negated ? " NOT BETWEEN " : " BETWEEN ") +
               expression_to_string(*expr.args[1]) + " AND " + expression_to_string(*expr.args[2]);
    case expr_kind_t::in_list: {
        std::string out = expression_to_string(*expr.args[0]);
        out += expr.negated ? " NOT IN (" : " IN (";
        for (std::size_t index = 1; index < expr.args.size(); ++index) {
            if (index > 1) out += ", ";
            out += expression_to_string(*expr.args[index]);
        }
        out += ")";
        return out;
    }
    default:
        return "?";
    }
}

void merope::collect_columns(const expr_t& expr, std::vector<std::string>& out) {
    if (expr.kind == expr_kind_t::column) {
        if (std::find(out.begin(), out.end(), expr.name) == out.end()) out.push_back(expr.name);
        return;
    }
    for (const expr_ptr& argument : expr.args) {
        if (argument != nullptr) collect_columns(*argument, out);
    }
}
