// Run in a disposable working directory: SettingsStore writes doa_settings.json.
#include "sigclass_presets.hpp"
#include "settings_store.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
using namespace sigpreset;
static std::string saved() {
    for (const auto& c : SettingsStore::apply_commands())
        if (c.starts_with("SIGCLASS_PRESETS:")) return c;
    assert(false); return {};
}
int main() {
    assert(parse("").empty());
    auto p = parse("p25p2,DIGITAL,9500;nxdn,DIGITAL,4000;p25p1,DIGITAL,12000");
    assert(p.at("nxdn").hz == 4000);
    assert(serialize(p) == "nxdn,DIGITAL,4000;p25p1,DIGITAL,12000;p25p2,DIGITAL,9500");
    assert(parse("x,AM,1000;y,WBFM,2400000").size() == 2);
    assert(parse("ssb,FACTORY,2700").at("ssb").demod == "FACTORY");
    int rejected = 0;
    for (auto bad : {"p25p1,DIGITAL,999","p25p1,DIGITAL,2400001","p25p1,USB,12000",
                     "p25p1,DIGITAL,12000x","p25p1,DIGITAL,12.5","p25p1,DIGITAL,",
                     "p25p1,DIGITAL,12000;",";p25p1,DIGITAL,12000",
                     "p25p1,DIGITAL,12000;p25p1,AM,12000","bad id,AM,12000"}) {
        try { (void)parse(bad); } catch (const std::invalid_argument&) { ++rejected; }
    }
    assert(rejected == 10);
    SettingsStore::load();
    assert(saved() == "SIGCLASS_PRESETS:");
    const std::string expected = "SIGCLASS_PRESETS:" + serialize(p);
    SettingsStore::record(expected); SettingsStore::flush();
    SettingsStore::load(); assert(saved() == expected);
    SettingsStore::begin_sandbox();
    SettingsStore::record("SIGCLASS_PRESETS:");
    SettingsStore::flush(); assert(saved() == "SIGCLASS_PRESETS:");
    SettingsStore::end_sandbox(); assert(saved() == expected);
    SettingsStore::load(); assert(saved() == expected);
    p.erase("nxdn"); SettingsStore::record("SIGCLASS_PRESETS:" + serialize(p));
    SettingsStore::flush(); SettingsStore::load();
    assert(saved() == "SIGCLASS_PRESETS:p25p1,DIGITAL,12000;p25p2,DIGITAL,9500");
    SettingsStore::reset_to_defaults(); assert(saved() == "SIGCLASS_PRESETS:");
    SettingsStore::load(); assert(saved() == "SIGCLASS_PRESETS:");
    std::cout << "PASS parser, canonical ordering, limits, persistence, sandbox, per-class and factory reset\n";
}
