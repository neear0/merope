// core/json_parser.h - the recursive descent parser behind json_parse().
//
// It is an implementation detail of json.cpp and has its own header only
// because a class declaration belongs in one. Callers use json_parse().
#pragma once

#include "json.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace merope {

class c_json_parser {
public:
    c_json_parser(std::string_view text, std::string& error);

    bool run(json_value_t& out);

private:
    static constexpr int k_max_depth = 64;

    bool fail(const char* message);
    void skip_space();
    bool literal(std::string_view word);

    bool parse_value(json_value_t& out, int depth);
    bool parse_object(json_value_t& out, int depth);
    bool parse_array(json_value_t& out, int depth);
    bool parse_string(std::string& out);
    bool parse_hex4(std::uint32_t& out);
    bool parse_number(json_value_t& out);

    std::string_view m_text;
    std::string&     m_error;
    std::size_t      m_cursor = 0;
};

} // namespace merope
