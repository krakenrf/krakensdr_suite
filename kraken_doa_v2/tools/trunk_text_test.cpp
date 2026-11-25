// The trunk messages' text and option rules (plugins/sdk/kraken_plugin.hpp
// kp::wire::clean_text / host_option - the plugin side and kraken_doa's
// decoder engine both use them, so a name is the same bytes everywhere):
// control characters (C0, DEL, C1), malformed UTF-8, overlong forms,
// surrogates, code points past U+10FFFF, cuts at a character boundary.
// Prints each failing case; exit code = the failures.
// Build and run (from kraken_doa_v2):
//   g++ -std=c++20 -O2 -Wall -Wextra -Iplugins/sdk tools/trunk_text_test.cpp -o trunk_text_test && ./trunk_text_test
#include "kraken_plugin.hpp"

#include <cstdio>
#include <string>

static int fails = 0;
static std::string hex(const std::string& s) {
    std::string o;
    char b[8];
    for (unsigned char c : s) {
        snprintf(b, sizeof b, "%02x ", c);
        o += b;
    }
    return o.empty() ? "(empty)" : o;
}
static void t(const std::string& in, size_t max, const std::string& want) {
    const std::string got = kp::wire::clean_text(in, max);
    if (got != want) {
        fails++;
        printf("FAIL clean_text(%s, %zu) = %s, want %s\n", hex(in).c_str(), max, hex(got).c_str(), hex(want).c_str());
    }
    if (kp::wire::clean_text(got, max) != got) {   // idempotent: kraken_doa's second pass changes nothing
        fails++;
        printf("FAIL not idempotent on %s\n", hex(got).c_str());
    }
}

int main() {
    const size_t M = 32;
    t("1-0412", M, "1-0412");
    t("", M, "");
    // C0, DEL, C1 (U+0080..U+009F as C2 80..C2 9F), with valid neighbours kept
    t("a\tb\nc\x1b[2Jd\x7f", M, "ab" "c[2Jd");
    t("\x1b[2J\xc2\x9b\xc3", M, "[2J");                 // the reviewer's case: ESC, CSI (U+009B), a cut-off 2-byte sequence
    t("x\xc2\x80y\xc2\x9fz", M, "xyz");
    t("\xc2\xa0nbsp \xc2\xbf", M, "\xc2\xa0nbsp \xc2\xbf");   // U+00A0, U+00BF stay
    t("\xc3\xa9t\xc3\xa9", M, "\xc3\xa9t\xc3\xa9");           // "ete" with accents
    t("\xe2\x82\xac 5", M, "\xe2\x82\xac 5");                 // euro sign
    t("\xf0\x9f\x93\xbb", M, "\xf0\x9f\x93\xbb");             // a 4-byte character
    t("\xf4\x8f\xbf\xbf", M, "\xf4\x8f\xbf\xbf");             // U+10FFFF, the last one
    // malformed: lone continuation bytes, bad leads, truncated sequences, a lead before ASCII
    t("a\x80\xbf" "b", M, "ab");
    t("\xc0\xaf\xc1\xbf" "ok", M, "ok");                       // overlong 2-byte (C0 / C1 leads)
    t("\xe0\x80\xaf" "ok", M, "ok");                           // overlong 3-byte
    t("\xf0\x80\x80\xaf" "ok", M, "ok");                       // overlong 4-byte
    t("\xed\xa0\x80\xed\xbf\xbf" "ok", M, "ok");               // surrogates D800, DFFF
    t("\xf4\x90\x80\x80\xf5\x80\x80\x80\xff\xfe" "ok", M, "ok");   // past U+10FFFF, F5.., FF, FE
    t("\xe2\x82" "A", M, "A");                                 // a 3-byte sequence cut short
    t("A\xe2\x82", M, "A");                                    // ... at the end
    t("\xc3" "A\xc3", M, "A");
    t("\xf0\x9f\x93", M, "");
    // the cut: at most max bytes, never inside a character
    t("abcdef", 4, "abcd");
    t("ab\xc3\xa9", 3, "ab");
    t("ab\xc3\xa9", 4, "ab\xc3\xa9");
    t("a\xe2\x82\xac", 3, "a");
    t("\xf0\x9f\x93\xbb\xf0\x9f\x93\xbb", 7, "\xf0\x9f\x93\xbb");
    t("\x01\x02" "abc", 3, "abc");                             // dropped bytes don't count
    t(std::string("a\0b", 3), M, "ab");                        // NUL (the wire's separator)
    // host_option: the OPTION keys the host sends itself
    for (const char* k : {"verbose", "log_raw", "video_wanted"})
        if (!kp::wire::host_option(k)) { fails++; printf("FAIL host_option(%s) = false\n", k); }
    for (const char* k : {"nac", "slot", "verbose2", "", "log"})
        if (kp::wire::host_option(k)) { fails++; printf("FAIL host_option(%s) = true\n", k); }
    printf("%d failures\n", fails);
    return fails;
}
