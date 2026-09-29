#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

// Escape a string for embedding inside a JSON string literal: quotes,
// backslashes and every control character below 0x20 (short forms where JSON
// has them, \u00XX otherwise). Shared by every hand-built JSON emitter.
inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Append Unicode code point cp as UTF-8.
inline void json_append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Four hex digits at json[p..p+3] -> value, or -1 if not all hex.
inline int json_hex4(const std::string& json, size_t p) {
    if (p + 4 > json.size()) return -1;
    int v = 0;
    for (size_t i = p; i < p + 4; i++) {
        const char c = json[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

// The reverse of json_escape for the flat objects we read (the settings file,
// web-mapper cloud messages): locates "key": <value> and returns the decoded
// string for a quoted value - every JSON escape, \uXXXX as UTF-8 including
// surrogate pairs - or the raw token for a bare number/bool. false if absent.
inline bool json_find(const std::string& json, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) p++;
    if (p >= json.size()) return false;
    if (json[p] != '"') {
        size_t e = p;
        while (e < json.size() && json[e] != ',' && json[e] != '}' &&
               !std::isspace(static_cast<unsigned char>(json[e]))) e++;
        out = json.substr(p, e - p);
        return true;
    }
    p++;
    std::string s;
    while (p < json.size() && json[p] != '"') {
        if (json[p] != '\\' || p + 1 >= json.size()) { s += json[p++]; continue; }
        const char c = json[p + 1];
        p += 2;
        switch (c) {
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u': {
                const int hi = json_hex4(json, p);
                if (hi < 0) { s += 'u'; break; }  // malformed: keep it literally
                p += 4;
                uint32_t cp = static_cast<uint32_t>(hi);
                if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: needs a low one
                    const int lo = (p + 1 < json.size() && json[p] == '\\' && json[p + 1] == 'u')
                                       ? json_hex4(json, p + 2) : -1;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(lo) - 0xDC00);
                        p += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;  // lone low surrogate
                }
                json_append_utf8(s, cp);
                break;
            }
            default: s += c; break;  // \", \\, \/ and anything else
        }
    }
    out = s;
    return true;
}
