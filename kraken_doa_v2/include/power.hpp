#pragma once
// ⏻ Shut down / ↻ Restart from the web page (docs/dev/power.md).
//
//   POWER:restart   restart the software: heimdall is asked to restart over
//                   its control port, then kraken_doa ends with the restart
//                   exit status its supervisor restarts on (run.sh, the
//                   Docker restart policy, krakensdr.exe)
//   POWER:reboot    reboot the computer
//   POWER:shutdown  power the computer off
//
// Before any of them a running IQ / audio recording and the DoA recording are
// finished, and the settings, the decoder log, incidents and noise floors are
// written. Every page is told ({"power":{...}}, also in the hello a page gets
// on connecting). Only a page's own WebSocket may send these: the settings
// replay, the AI assistant, the web mapper and the scanners are refused, and
// none of them is persisted.
//
// Environment: KRAKEN_NO_POWER=1 (no buttons), KRAKEN_POWER=restart,reboot,
// shutdown (the actions offered at most), KRAKEN_POWER_DRY_RUN=1 (log the
// command instead of running it), KRAKEN_RESTART_EXIT=<2..125> (set by the
// supervisor that restarts kraken_doa on that exit status).
#include <string>
#include <string_view>

namespace power {

// main(), once: reads the environment, then finds out on a worker thread
// which of reboot / shutdown this user may run (pages are told when it is done)
void init();

// {"power":{"actions":[...],"busy":"",...}} - a connecting page's copy
std::string state_message();

bool is_command(std::string_view m);   // "POWER:..."

// uWS loop thread. from_page = it came in on a browser's WebSocket (and not
// from the settings replay, the assistant, ...). "" = started, else why not.
std::string handle_command(std::string_view m, bool from_page);

// main's exit status: 0, or KRAKEN_RESTART_EXIT after an accepted restart
int exit_code();

}  // namespace power
