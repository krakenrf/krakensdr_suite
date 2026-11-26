#pragma once
// Bounded canonical settings suffix; empty means model defaults.
#include "config.hpp"
#include <charconv>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
namespace sigpreset {
struct Preset { std::string demod; int hz; };
using Overrides = std::map<std::string, Preset>;
inline constexpr size_t MAX_ENTRIES = 256, MAX_BYTES = 32768;
inline bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
inline bool valid_demod(std::string_view s) {
    return s == "FACTORY" || s == "NBFM" || s == "WBFM" || s == "AM" || s == "DIGITAL";
}
inline Overrides parse(std::string_view text) {
    Overrides out;
    if (text.size() > MAX_BYTES) throw std::invalid_argument("preset list too large");
    while (!text.empty()) {
        const auto end = text.find(';');
        const auto row = text.substr(0, end);
        const auto a = row.find(','), b = a == row.npos ? row.npos : row.find(',', a + 1);
        if (a == row.npos || b == row.npos) throw std::invalid_argument("expected class,demod,bandwidth_hz");
        const auto id = row.substr(0,a), mode = row.substr(a+1,b-a-1), bw = row.substr(b+1);
        int hz = 0;
        const auto r = std::from_chars(bw.data(), bw.data()+bw.size(), hz);
        if (!valid_id(id) || !valid_demod(mode) || r.ec != std::errc{} || r.ptr != bw.data()+bw.size()
            || hz < VFO_BW_MIN_HZ || hz > VFO_BW_MAX_HZ)
            throw std::invalid_argument("invalid Signal ID preset");
        if (!out.emplace(std::string(id), Preset{std::string(mode),hz}).second || out.size() > MAX_ENTRIES)
            throw std::invalid_argument("duplicate class or too many presets");
        if (end == text.npos) break;
        text.remove_prefix(end+1);
        if (text.empty()) throw std::invalid_argument("trailing preset separator");
    }
    return out;
}
inline std::string serialize(const Overrides& rows) {
    if (rows.size() > MAX_ENTRIES) throw std::invalid_argument("too many presets");
    std::string out;
    for (const auto& [id,p] : rows) {
        if (!out.empty()) out += ';';
        out += id + "," + p.demod + "," + std::to_string(p.hz);
    }
    if (out.size() > MAX_BYTES) throw std::invalid_argument("preset list too large");
    return out;
}
} // namespace sigpreset
