// core/json.h - a small dependency free JSON DOM.
// The plan the AI returns and the confirmed schema we persist are both JSON,
// and the project pulls in no third party libraries, so this lives here.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace merope {

struct json_value_t;

using json_array_t  = std::vector<json_value_t>;
// Object members keep insertion order so a round trip stays readable.
using json_object_t = std::vector<std::pair<std::string, json_value_t>>;

enum class json_kind_t : std::uint8_t { null, boolean, number, string, array, object };

struct json_value_t {
    json_kind_t   kind = json_kind_t::null;
    bool          boolean_value = false;
    double        number_value  = 0.0;
    std::string   string_value;
    json_array_t  array_value;
    json_object_t object_value;

    static json_value_t make_null();
    static json_value_t make_bool(bool value);
    static json_value_t make_number(double value);
    static json_value_t make_string(std::string value);
    static json_value_t make_array(json_array_t value = {});
    static json_value_t make_object(json_object_t value = {});

    bool is_null()   const noexcept { return kind == json_kind_t::null; }
    bool is_object() const noexcept { return kind == json_kind_t::object; }
    bool is_array()  const noexcept { return kind == json_kind_t::array; }
    bool is_string() const noexcept { return kind == json_kind_t::string; }
    bool is_number() const noexcept { return kind == json_kind_t::number; }
    bool is_bool()   const noexcept { return kind == json_kind_t::boolean; }

    // Object lookup. Returns nullptr when absent or when this is not an object.
    const json_value_t* find(std::string_view key) const noexcept;

    // Typed reads with a fallback, for optional plan fields.
    std::string  string_or(std::string_view key, std::string fallback) const;
    double       number_or(std::string_view key, double fallback) const noexcept;
    bool         bool_or(std::string_view key, bool fallback) const noexcept;
    std::int64_t int_or(std::string_view key, std::int64_t fallback) const noexcept;

    void set(std::string key, json_value_t value);
};

// Parses `text`. On failure returns false and fills `error` with a message that
// names the byte offset, so a malformed AI response is debuggable.
bool json_parse(std::string_view text, json_value_t& out, std::string& error);

// `indent` of 0 produces a single line.
std::string json_serialize(const json_value_t& value, int indent = 2);

} // namespace merope
