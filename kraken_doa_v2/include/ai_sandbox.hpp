#pragma once

// Code the AI assistant wrote never runs outside a sandbox.
//
// The assistant may edit decoder plugins (shipped ones too - the user's
// call); its own build / test commands run in Claude Code's bwrap sandbox. But
// kraken_doa runs plugins itself - the live decoders, `decoder --info` at a
// rescan, the debug playback's test-signal generators - and those would run
// the assistant's code with the receiver's full rights. So kraken_doa keeps
// its OWN list of the plugins the assistant changed (ai/state/
// sandboxed_plugins.json - the agent can't write ai/state): at the end of
// every assistant turn, each plugin folder with a file whose ctime (which no
// process can set back, unlike the mtime) is newer than the turn's start goes
// on it; during a turn, a changed folder counts at once (a decoder rebuilt
// mid-turn restarts before the turn ends). Such plugins run under bwrap:
// read-only filesystem, no network, a private /tmp. `python3 ai/kraken_ai.py
// trust <id>` (on the Pi) takes one off the list.
//
// Windows has no bwrap (and no other sandbox here): available() is false, so
// a plugin on the list doesn't run at all until the user trusts it (`py -3
// ai/kraken_ai.py trust <id>` on the PC); the list is kept the same way, by
// the files' modification / creation times (Windows has no ctime - the
// assistant has no shell there, so nothing can set a time back either).

#include <cstdint>
#include <string>
#include <vector>

namespace aisandbox {

// bwrap is installed and works here (checked once); always false on Windows
bool available();

// How to get going when something must run sandboxed and can't be - the end
// of the refusals: "... it runs only in a sandbox - <NO_SANDBOX_FIX>"
#ifdef _WIN32
inline constexpr const char* NO_SANDBOX_FIX =
    "Windows has none: check what the assistant changed, then run in kraken_doa_v2: py -3 ai/kraken_ai.py trust <id>";
#else
inline constexpr const char* NO_SANDBOX_FIX = "install bubblewrap (bwrap)";
#endif

// the plugin folder (plugins/<id>) must run sandboxed
bool must_sandbox_dir(const std::string& plugin_dir);
// the same for an executable inside one (plugins/<id>/build/decoder); false
// for anything that isn't a plugin (the Voice to Text helper ...)
bool must_sandbox_exe(const std::string& exe);
bool any_marked();

// AiManager: an assistant turn starts / ends (the end adds the folders it
// changed to the list)
void turn_started(int64_t wall_ms);
void turn_finished();
// startup: a turn that never finished (kraken_doa stopped mid-turn - its
// start is in ai/state/turn) is finished now
void recover();

// argv wrapped in bwrap (rw = folders it may write besides a private /tmp);
// empty when bwrap isn't available (always on Windows)
std::vector<std::string> wrap(const std::vector<std::string>& argv, const std::vector<std::string>& rw = {});

std::string list_json();   // ["id", ...]

}  // namespace aisandbox
