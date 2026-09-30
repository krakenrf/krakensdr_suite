#pragma once

#include <string>
#include <string_view>
#include <vector>

// Backend persistence of user settings.
//
// A canonical schema (settings_store.cpp) lists every remembered setting with
// its command prefix, JSON type and hardcoded default. The full set of settings
// is serialized to a JSON file (doa_settings.json) - seeded with defaults on
// first run - and restored on startup by replaying each setting through the
// normal control handler. Persistence lives on the backend, so it is shared by
// every browser rather than being per-browser.
namespace SettingsStore {
    // Populate the in-memory store from the JSON file, filling any missing keys
    // with their defaults and (re)writing the file so it always holds the full
    // set. Creates the file with all defaults on first run. Call once at startup.
    void load();

    // Every setting as a "PREFIX:value" command, in schema order, for replay.
    std::vector<std::string> apply_commands();

    // Record the latest value of a setting command (e.g. "TOPOLOGY:ULA"). A
    // no-op for commands that aren't in the schema.
    void record(std::string_view command);

    // True if the command's prefix is a remembered setting in the schema.
    bool is_known(std::string_view command);

    // Reset every setting to its hardcoded default, write the file immediately,
    // and return the default commands so the caller can apply them live.
    std::vector<std::string> reset_to_defaults();

    // Start / stop the debounced background writer thread.
    void start();
    void stop();

    // Write immediately if there are unsaved changes.
    void flush();
}
