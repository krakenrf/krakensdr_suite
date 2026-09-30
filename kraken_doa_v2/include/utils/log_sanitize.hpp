#pragma once

// Terminal-safe logging. Log lines carry strings that network clients choose
// (WebSocket commands, close reasons, filenames, settings values). Written raw,
// an ESC sequence in one of them is executed by the terminal that shows the
// log - tmux pane, TUI log view, or `tail` of a headless log file - and can
// recolor or rewrite the screen, retitle the window, or (on some terminals)
// answer back into the shell. Everything written to std::cout / std::cerr
// therefore goes through SanitizingBuf (installed in main) or, while the TUI
// dashboard captures output, through sanitize_for_terminal() per line.
//
// Kept: printable ASCII, '\n', '\t' and valid UTF-8. Replaced with '?': other
// C0 controls (ESC, '\r', BEL, ...), DEL, C1 controls (U+0080-U+009F) and
// bytes that aren't valid UTF-8 (a raw 0x9B is CSI on an 8-bit terminal).
// The apps' own log lines never contain control sequences - the dashboards
// write their escape codes straight to the terminal fd, not through cout.

#include <streambuf>
#include <string>
#include <string_view>

namespace log_sanitize_detail {
// Length of the valid UTF-8 sequence at s[i] (>= 2 bytes), or 0 if invalid.
inline size_t utf8_len(std::string_view s, size_t i) {
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
}  // namespace log_sanitize_detail

inline std::string sanitize_for_terminal(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) { out += static_cast<char>(c); i++; continue; }
        if (c < 0x80) { out += '?'; i++; continue; }  // C0 control / DEL
        const size_t n = log_sanitize_detail::utf8_len(s, i);
        // U+0080-U+009F (C1 controls) encode as C2 80..C2 9F
        if (n == 0 || (c == 0xC2 && static_cast<unsigned char>(s[i + 1]) <= 0x9F)) { out += '?'; i += n ? n : 1; continue; }
        out.append(s.substr(i, n));
        i += n;
    }
    return out;
}

// Forwards to another streambuf a line at a time, sanitized. Buffers per
// thread (and per stream), so concurrent writers can't interleave inside a
// line and a UTF-8 sequence is never split between two sanitize calls. A
// flush (std::endl, std::flush, cerr's unitbuf) forwards the partial line.
class SanitizingBuf : public std::streambuf {
public:
    SanitizingBuf(std::streambuf* dest, int slot) : dest_(dest), slot_(slot) {}

protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        const char ch = static_cast<char>(c);
        put(&ch, 1);
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        put(s, static_cast<size_t>(n));
        return n;
    }
    int sync() override {
        emit();
        return dest_->pubsync();
    }

private:
    std::string& partial() {
        thread_local std::string buf[2];  // one per stream (cout / cerr)
        return buf[slot_ & 1];
    }
    void put(const char* s, size_t n) {
        std::string& p = partial();
        for (size_t i = 0; i < n; i++) {
            p += s[i];
            if (s[i] == '\n' || p.size() >= 4096) emit();
        }
    }
    void emit() {
        std::string& p = partial();
        if (p.empty()) return;
        const std::string clean = sanitize_for_terminal(p);
        dest_->sputn(clean.data(), static_cast<std::streamsize>(clean.size()));
        p.clear();
    }

    std::streambuf* dest_;
    int slot_;
};
