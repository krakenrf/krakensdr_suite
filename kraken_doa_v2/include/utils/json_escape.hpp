#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

// Length of the valid UTF-8 sequence starting at s[i] (2-4), or 0 if invalid
// (overlongs, surrogates and > U+10FFFF rejected).
inline size_t json_utf8_len(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    size_t n; unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) { n = 3; if (c == 0xE0) lo = 0xA0; else if (c == 0xED) hi = 0x9F; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 4; if (c == 0xF0) lo = 0x90; else if (c == 0xF4) hi = 0x8F; }
    else return 0;
    if (i + n > s.size() || b(i + 1) < lo || b(i + 1) > hi) return 0;
    for (size_t k = 2; k < n; k++)
        if (b(i + k) < 0x80 || b(i + k) > 0xBF) return 0;
    return n;
}

// Escape a string for embedding inside a JSON string literal: quotes,
// backslashes and every control character below 0x20 (short forms where JSON
// has them, \u00XX otherwise). Shared by every hand-built JSON emitter.
// Invalid UTF-8 becomes \ufffd: these strings go to browsers as WebSocket
// TEXT frames, and a browser drops the connection on one invalid byte - a
// station ID pushed by the cloud, a hand-edited settings value or a recording
// filename made outside the app used to disconnect every browser on every
// (re)connect.
inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (static_cast<unsigned char>(c) >= 0x80) {
            const size_t n = json_utf8_len(s, i);
            if (n == 0) { out += "\\ufffd"; continue; }
            out.append(s.substr(i, n));
            i += n - 1;
            continue;
        }
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

// Decode the JSON string whose opening quote is at json[p]: every escape,
// \uXXXX as UTF-8 including surrogate pairs. Returns the position just past
// the closing quote (json.size() if unterminated).
inline size_t json_read_string(const std::string& json, size_t p, std::string& out) {
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
    return p < json.size() ? p + 1 : p;
}

// The reverse of json_escape for the flat objects we read (the settings file,
// web-mapper cloud messages): locates "key": <value> and returns the decoded
// string for a quoted value or the raw token for a bare number/bool. false if
// absent. Only a KEY of the top-level object matches: the scan walks string
// tokens, so a string VALUE that happens to equal the key name (a station ID
// of "latitude", say) - or the key text inside another string or a nested
// object - is skipped. (A plain text search returned the token after the
// first such occurrence instead.)
inline bool json_find(const std::string& json, const std::string& key, std::string& out) {
    int depth = 0;
    size_t p = 0;
    while (p < json.size()) {
        const char c = json[p];
        if (c == '"') {
            std::string tok;
            p = json_read_string(json, p, tok);
            size_t q = p;
            while (q < json.size() && std::isspace(static_cast<unsigned char>(json[q]))) q++;
            if (depth != 1 || q >= json.size() || json[q] != ':' || tok != key) continue;
            q++;  // the value
            while (q < json.size() && std::isspace(static_cast<unsigned char>(json[q]))) q++;
            if (q >= json.size()) return false;
            if (json[q] == '"') {
                json_read_string(json, q, out);
                return true;
            }
            size_t e = q;
            while (e < json.size() && json[e] != ',' && json[e] != '}' &&
                   !std::isspace(static_cast<unsigned char>(json[e]))) e++;
            out = json.substr(q, e - q);
            return true;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        p++;
    }
    return false;
}
