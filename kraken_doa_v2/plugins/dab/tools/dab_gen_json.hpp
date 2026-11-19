// Minimal JSON reader for dab_gen's scene files (objects, arrays, strings,
// numbers, true / false / null). Plain C++20, no locale-dependent parsing of
// the structure; numbers go through std::strtod.
#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dabjson {

struct Value {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::string> keys;   // object: keys[i] -> vals[i] (two vectors: a
    std::vector<Value> vals;         // vector of an incomplete type is allowed, a pair is not)

    const Value* get(const std::string& k) const {
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == k) return &vals[i];
        return nullptr;
    }
    bool has(const std::string& k) const { return get(k) != nullptr; }
    double num(const std::string& k, double def) const {
        const Value* v = get(k);
        if (!v || v->kind == Null) return def;
        if (v->kind == Num) return v->n;
        if (v->kind == Bool) return v->b ? 1 : 0;
        if (v->kind == Str) return std::strtod(v->s.c_str(), nullptr);
        throw std::runtime_error("\"" + k + "\" is not a number");
    }
    std::string str(const std::string& k, const std::string& def) const {
        const Value* v = get(k);
        if (!v || v->kind == Null) return def;
        if (v->kind == Str) return v->s;
        if (v->kind == Num) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.17g", v->n);
            return buf;
        }
        throw std::runtime_error("\"" + k + "\" is not a string");
    }
};

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}
    Value parse() {
        Value v = value();
        ws();
        if (p_ != t_.size()) fail("trailing characters");
        return v;
    }

private:
    const std::string& t_;
    size_t p_ = 0;
    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(std::string("JSON: ") + what + " at offset " + std::to_string(p_));
    }
    void ws() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) p_++;
    }
    bool lit(const char* s) {
        size_t n = std::char_traits<char>::length(s);
        if (t_.compare(p_, n, s) == 0) {
            p_ += n;
            return true;
        }
        return false;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
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
    unsigned hex4() {
        if (p_ + 4 > t_.size()) fail("bad \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }
    std::string string() {
        if (t_[p_] != '"') fail("expected string");
        p_++;
        std::string out;
        while (true) {
            if (p_ >= t_.size()) fail("unterminated string");
            char c = t_[p_++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ >= t_.size()) fail("bad escape");
            char e = t_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && lit("\\u")) {
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
        return out;
    }
    Value value() {
        ws();
        if (p_ >= t_.size()) fail("unexpected end");
        Value v;
        char c = t_[p_];
        if (c == '{') {
            v.kind = Value::Obj;
            p_++;
            ws();
            if (p_ < t_.size() && t_[p_] == '}') {
                p_++;
                return v;
            }
            while (true) {
                ws();
                std::string k = string();
                ws();
                if (p_ >= t_.size() || t_[p_] != ':') fail("expected ':'");
                p_++;
                Value x = value();
                bool replaced = false;
                for (size_t i = 0; i < v.keys.size(); i++)
                    if (v.keys[i] == k) {
                        v.vals[i] = x;
                        replaced = true;
                    }
                if (!replaced) {
                    v.keys.push_back(k);
                    v.vals.push_back(x);
                }
                ws();
                if (p_ < t_.size() && t_[p_] == ',') {
                    p_++;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == '}') {
                    p_++;
                    break;
                }
                fail("expected ',' or '}'");
            }
        } else if (c == '[') {
            v.kind = Value::Arr;
            p_++;
            ws();
            if (p_ < t_.size() && t_[p_] == ']') {
                p_++;
                return v;
            }
            while (true) {
                v.a.push_back(value());
                ws();
                if (p_ < t_.size() && t_[p_] == ',') {
                    p_++;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == ']') {
                    p_++;
                    break;
                }
                fail("expected ',' or ']'");
            }
        } else if (c == '"') {
            v.kind = Value::Str;
            v.s = string();
        } else if (lit("true")) {
            v.kind = Value::Bool;
            v.b = true;
        } else if (lit("false")) {
            v.kind = Value::Bool;
        } else if (lit("null")) {
            v.kind = Value::Null;
        } else {
            size_t q = p_;
            while (q < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[q])) || t_[q] == '-' || t_[q] == '+' ||
                                     t_[q] == '.' || t_[q] == 'e' || t_[q] == 'E'))
                q++;
            if (q == p_) fail("unexpected character");
            std::string num = t_.substr(p_, q - p_);
            char* end = nullptr;
            v.kind = Value::Num;
            v.n = std::strtod(num.c_str(), &end);
            if (!end || *end) fail("bad number");
            p_ = q;
        }
        return v;
    }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

}  // namespace dabjson
