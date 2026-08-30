#include "encoding.h"

#include <array>
#include <cctype>
#include <cstdint>

// Code points for bytes 0x80..0xFF. Undefined slots map to U+FFFD.
static constexpr std::array<std::uint16_t, 128> k_cp1250 = {
    0x20AC, 0xFFFD, 0x201A, 0xFFFD, 0x201E, 0x2026, 0x2020, 0x2021,
    0xFFFD, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0xFFFD, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
    0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
    0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9
};

static constexpr std::array<std::uint16_t, 128> k_iso_8859_2 = {
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
    0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7,
    0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
    0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7,
    0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9
};

// Scans for well formed UTF-8. Returns false on the first invalid sequence.
static bool is_valid_utf8(const unsigned char* data, std::size_t size, bool& saw_multibyte) noexcept {
    saw_multibyte = false;
    std::size_t index = 0;
    while (index < size) {
        const unsigned char lead = data[index];
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (lead < 0x80) {
            ++index;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1; code = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2; code = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3; code = lead & 0x07u;
        } else {
            return false;
        }
        // A truncated sequence at the very end of the sample is not evidence of
        // a broken file, only of where we cut the sample.
        if (index + extra >= size) return true;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cont = data[index + k];
            if ((cont & 0xC0) != 0x80) return false;
            code = (code << 6) | (cont & 0x3Fu);
        }
        if (extra == 1 && code < 0x80)    return false;  // overlong
        if (extra == 2 && code < 0x800)   return false;
        if (extra == 3 && code < 0x10000) return false;
        if (code > 0x10FFFF)              return false;
        if (code >= 0xD800 && code <= 0xDFFF) return false;
        saw_multibyte = true;
        index += extra + 1;
    }
    return true;
}

// Counts how many high bytes decode to a letter that is actually used in
// Central European text. The winning table is the better guess.
static int score_table(const std::array<std::uint16_t, 128>& table,
                const unsigned char* data, std::size_t size) noexcept {
    static constexpr std::uint16_t k_letters[] = {
        0x00E1, 0x010D, 0x010F, 0x00E9, 0x011B, 0x00ED, 0x013E, 0x013A,
        0x0148, 0x00F3, 0x00F4, 0x0159, 0x0161, 0x0165, 0x00FA, 0x016F,
        0x00FD, 0x017E, 0x0105, 0x0107, 0x0119, 0x0142, 0x015B, 0x017A,
        0x017C, 0x00C1, 0x010C, 0x0160, 0x017D, 0x0164
    };
    int score = 0;
    for (std::size_t index = 0; index < size; ++index) {
        if (data[index] < 0x80) continue;
        const std::uint16_t code = table[data[index] - 0x80];
        for (const std::uint16_t letter : k_letters) {
            if (code == letter) { ++score; break; }
        }
    }
    return score;
}

static std::string transcode_single_byte(const std::string& input,
                                  const std::array<std::uint16_t, 128>& table) {
    std::string out;
    out.reserve(input.size() + input.size() / 4);
    for (const char raw : input) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        if (byte < 0x80) {
            out.push_back(raw);
        } else {
            merope::append_utf8(out, table[byte - 0x80]);
        }
    }
    return out;
}

void merope::append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

const char* merope::to_string(encoding_t encoding) noexcept {
    switch (encoding) {
    case encoding_t::utf8:         return "UTF-8";
    case encoding_t::utf8_bom:     return "UTF-8 (BOM)";
    case encoding_t::windows_1250: return "windows-1250";
    case encoding_t::iso_8859_2:   return "ISO-8859-2";
    case encoding_t::utf16_le:     return "UTF-16LE";
    case encoding_t::utf16_be:     return "UTF-16BE";
    case encoding_t::unknown:
    default:                       return "unknown";
    }
}

bool merope::encoding_from_string(std::string_view text, encoding_t& out) noexcept {
    static constexpr encoding_t k_known[] = {
        encoding_t::utf8, encoding_t::utf8_bom, encoding_t::windows_1250,
        encoding_t::iso_8859_2, encoding_t::utf16_le, encoding_t::utf16_be,
        encoding_t::unknown
    };
    for (const encoding_t candidate : k_known) {
        const std::string_view name = to_string(candidate);
        if (name.size() != text.size()) continue;
        bool same = true;
        for (std::size_t index = 0; index < name.size() && same; ++index) {
            same = std::tolower(static_cast<unsigned char>(name[index])) ==
                   std::tolower(static_cast<unsigned char>(text[index]));
        }
        if (same) {
            out = candidate;
            return true;
        }
    }
    return false;
}

bool merope::is_byte_oriented(encoding_t encoding) noexcept {
    return encoding == encoding_t::utf8 || encoding == encoding_t::utf8_bom ||
           encoding == encoding_t::windows_1250 || encoding == encoding_t::iso_8859_2;
}

std::size_t merope::bom_length(encoding_t encoding) noexcept {
    switch (encoding) {
    case encoding_t::utf8_bom: return 3;
    case encoding_t::utf16_le:
    case encoding_t::utf16_be: return 2;
    default:                   return 0;
    }
}

merope::encoding_t merope::detect_encoding(const char* data, std::size_t size) noexcept {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
    if (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return encoding_t::utf8_bom;
    }
    if (size >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) return encoding_t::utf16_le;
    if (size >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) return encoding_t::utf16_be;

    bool saw_multibyte = false;
    if (is_valid_utf8(bytes, size, saw_multibyte)) {
        return encoding_t::utf8;
    }

    const int cp1250_score = score_table(k_cp1250, bytes, size);
    const int latin2_score = score_table(k_iso_8859_2, bytes, size);
    // windows-1250 wins ties: it is far more common in practice than Latin-2.
    return latin2_score > cp1250_score ? encoding_t::iso_8859_2 : encoding_t::windows_1250;
}

std::string merope::transcode_to_utf8(const std::string& input, encoding_t encoding) {
    switch (encoding) {
    case encoding_t::windows_1250: return transcode_single_byte(input, k_cp1250);
    case encoding_t::iso_8859_2:   return transcode_single_byte(input, k_iso_8859_2);
    default:                       return input;
    }
}

