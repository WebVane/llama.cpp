#include "license/Jcs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace visio::license {
namespace {

using json = nlohmann::json;

// Decode a UTF-8 string to a sequence of UTF-16 code units (as uint32_t for
// uniform compare — but each value fits in 16 bits).  Used to compare object
// keys per the JCS rule (UTF-16 code-unit order, matching Java's
// String.compareTo).  For ASCII keys (the schema we use) this is equivalent
// to byte-order compare; the implementation handles general UTF-8 for
// future-proofness.
std::vector<uint32_t> utf8_to_utf16_units(std::string_view s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        const uint8_t b0 = static_cast<uint8_t>(s[i]);
        if (b0 < 0x80) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 >= s.size()) throw std::runtime_error("Jcs: invalid UTF-8 (2-byte truncated)");
            cp = ((b0 & 0x1F) << 6) |
                 (static_cast<uint8_t>(s[i + 1]) & 0x3F);
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            if (i + 2 >= s.size()) throw std::runtime_error("Jcs: invalid UTF-8 (3-byte truncated)");
            cp = ((b0 & 0x0F) << 12) |
                 ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(s[i + 2]) & 0x3F);
            i += 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            if (i + 3 >= s.size()) throw std::runtime_error("Jcs: invalid UTF-8 (4-byte truncated)");
            cp = ((b0 & 0x07) << 18) |
                 ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 12) |
                 ((static_cast<uint8_t>(s[i + 2]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(s[i + 3]) & 0x3F);
            i += 4;
        } else {
            throw std::runtime_error("Jcs: invalid UTF-8 lead byte");
        }
        // Encode to UTF-16: BMP → 1 unit; astral → surrogate pair.
        if (cp <= 0xFFFF) {
            out.push_back(cp);
        } else {
            const uint32_t v = cp - 0x10000;
            out.push_back(0xD800 | (v >> 10));
            out.push_back(0xDC00 | (v & 0x3FF));
        }
    }
    return out;
}

bool key_less_utf16(std::string_view a, std::string_view b) {
    auto ua = utf8_to_utf16_units(a);
    auto ub = utf8_to_utf16_units(b);
    return std::lexicographical_compare(ua.begin(), ua.end(),
                                        ub.begin(), ub.end());
}

void emit_hex4_lower(std::string& out, uint32_t v) {
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('\\');
    out.push_back('u');
    out.push_back(hex[(v >> 12) & 0xF]);
    out.push_back(hex[(v >> 8) & 0xF]);
    out.push_back(hex[(v >> 4) & 0xF]);
    out.push_back(hex[v & 0xF]);
}

// Emit `"…"` for a UTF-8 string per RFC 8785 §3.2.2.2 + DEL extension.
// Non-ASCII bytes (>=0x80) are passed through verbatim (UTF-8 is already
// the wire encoding) — we only validate that they form well-formed UTF-8
// when we have to (key sorting); for value strings, we let invalid UTF-8
// through unchanged because re-encoding it would be lossy.  In practice
// every .lic field value is ASCII so this never matters.
void emit_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (std::size_t i = 0; i < s.size(); ++i) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case 0x08: out += "\\b";  break;
            case 0x09: out += "\\t";  break;
            case 0x0A: out += "\\n";  break;
            case 0x0C: out += "\\f";  break;
            case 0x0D: out += "\\r";  break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    emit_hex4_lower(out, c);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

// ECMAScript Number.toString for integer-valued numbers only — which is
// all the license schema needs.  Non-integer doubles throw; the caller
// must reject the input.  We accept any nlohmann numeric storage type
// (int, unsigned, float) as long as the value is an exact integer in
// the safe int64 range.
void emit_number(std::string& out, const json& v) {
    if (v.is_number_integer()) {
        if (v.is_number_unsigned()) {
            out += std::to_string(v.get<uint64_t>());
        } else {
            out += std::to_string(v.get<int64_t>());
        }
        return;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (!std::isfinite(d)) {
            throw std::runtime_error("Jcs: NaN/Inf not allowed");
        }
        // Integer-valued double: emit as integer.  Range check keeps us
        // inside the JS-safe range (2^53 - 1).
        const double trunc = std::trunc(d);
        if (trunc == d && std::abs(d) < 9.007199254740992e15) {
            out += std::to_string(static_cast<int64_t>(trunc));
            return;
        }
        // Non-integer floats are out of spec for the .lic schema — fail loud.
        throw std::runtime_error(
            "Jcs: non-integer numbers not supported by the .lic schema");
    }
    throw std::runtime_error("Jcs: unsupported numeric type");
}

void emit(std::string& out, const json& v);

void emit_object(std::string& out, const json& obj) {
    // Collect keys, sort by UTF-16 code-unit order, then emit.
    std::vector<std::string_view> keys;
    keys.reserve(obj.size());
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        keys.emplace_back(it.key());
    }
    std::sort(keys.begin(), keys.end(),
              [](std::string_view a, std::string_view b) {
                  return key_less_utf16(a, b);
              });
    out.push_back('{');
    bool first = true;
    for (auto k : keys) {
        if (!first) out.push_back(',');
        first = false;
        emit_string(out, k);
        out.push_back(':');
        emit(out, obj.at(std::string(k)));
    }
    out.push_back('}');
}

void emit_array(std::string& out, const json& arr) {
    out.push_back('[');
    bool first = true;
    for (const auto& el : arr) {
        if (!first) out.push_back(',');
        first = false;
        emit(out, el);
    }
    out.push_back(']');
}

void emit(std::string& out, const json& v) {
    switch (v.type()) {
        case json::value_t::null:    out += "null"; break;
        case json::value_t::boolean: out += v.get<bool>() ? "true" : "false"; break;
        case json::value_t::string:  emit_string(out, v.get_ref<const std::string&>()); break;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float:
            emit_number(out, v);
            break;
        case json::value_t::object: emit_object(out, v); break;
        case json::value_t::array:  emit_array(out, v);  break;
        default:
            throw std::runtime_error("Jcs: unsupported json value type");
    }
}

}  // namespace

std::string jcs_canonicalize(const json& v) {
    std::string out;
    // Heuristic reserve — license payloads land around 250-500 bytes.
    out.reserve(512);
    emit(out, v);
    return out;
}

}  // namespace visio::license
