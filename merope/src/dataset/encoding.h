// dataset/encoding.h - encoding detection and transcoding to UTF-8 (spec 4.1)
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace merope {

enum class encoding_t {
    utf8,          // valid UTF-8, no BOM
    utf8_bom,      // valid UTF-8 with a leading BOM
    windows_1250,  // Central European, the usual suspect for SK/CZ exports
    iso_8859_2,    // Latin-2
    utf16_le,      // detected, not supported for streaming reads
    utf16_be,      // detected, not supported for streaming reads
    unknown
};

const char* to_string(encoding_t encoding) noexcept;

bool encoding_from_string(std::string_view text, encoding_t& out) noexcept;

// True when the engine can stream the file byte-wise without transcoding.
bool is_byte_oriented(encoding_t encoding) noexcept;

// Number of bytes of BOM at the start of the buffer for this encoding.
std::size_t bom_length(encoding_t encoding) noexcept;

encoding_t detect_encoding(const char* data, std::size_t size) noexcept;

// Transcodes one line/field from the source encoding into UTF-8. For UTF-8
// input this is a copy; invalid sequences are replaced with U+FFFD.
std::string transcode_to_utf8(const std::string& input, encoding_t encoding);

// Appends a code point to a UTF-8 string.
void append_utf8(std::string& out, std::uint32_t code_point);

}
