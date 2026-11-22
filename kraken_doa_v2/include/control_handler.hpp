#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ControlHandler {
public:
    static void handle_websocket_message(std::string_view message);
    // A command that came in on a page's own WebSocket (websocket_server.cpp
    // only): the same dispatch, and the power commands (power.hpp) are taken
    // only from here - never from the settings replay, the AI assistant, the
    // web mapper or the scanners, which use the two calls around it
    static void handle_page_message(std::string_view message);
    // The same, for callers that need to know whether it was taken (the AI
    // assistant's control tools, ai_mcp.cpp): "" = applied, else why it was
    // refused (CommandRejected, a debug playback's refusal, malformed). Same
    // thread rule: the uWS loop only
    static std::string apply_command(std::string_view message);
    static void send_control_command(const std::string& cmd);

    // Settings-sync messages ({"sync_cmd":...}) sent to a newly connected
    // browser so its UI matches the current shared state.
    static std::vector<std::string> get_connect_sync_messages();

    // Replay the persisted settings file through the normal command path. Call
    // once at startup, after the decimators exist and the server is reachable,
    // ON THE uWS LOOP THREAD (loop->defer) like every other dispatch.
    static void apply_persisted_settings();

    // Client side of a wideband-scan mode change: sets wideband_mode_enabled
    // and parks / restores DoA (MUSIC can't run while the tuners are spread).
    // Idempotent - only an actual transition saves or restores the DoA state.
    // Thread-safe (the discrete scanner's lock/resume worker calls it too).
    // Returns whether the mode changed. Sends nothing to heimdall.
    static bool apply_wideband_mode_state(bool enable);

    // The general form: 0 coherent, 1 wideband scan, 2 independent tuners.
    // Leaving coherent parks DoA, returning restores it; broadcasts
    // {"operating_mode":...} on a change. Sends nothing to heimdall.
    // Thread-safe; returns whether the mode changed. ("wideband off" via
    // apply_wideband_mode_state(false) only leaves the wideband scan.)
    static bool apply_operating_mode_state(int mode);
    // {"operating_mode":{mode, tuners, generic_rtlsdr[, error]}}: error = why
    // a mode change was refused (the pages show it)
    static std::string build_operating_mode_json(const std::string& error = {});

    // Data receiver: heimdall's mode from the packet header (bits 10/11 of
    // the phase-state field). A mismatch that outlasts a client-initiated
    // change is adopted (heimdall restored its saved mode after a restart,
    // or another control client switched it). Thread-safe.
    static void note_server_mode(int mode);

    // Sweep mode (sweep.hpp). Data receiver, every packet: heimdall's mode
    // from the header (3 = sweep, flag 0x2000) - after OPERATING_MODE:sweep a
    // heimdall that never shows the flag (an older one: it answered an error)
    // gets the client back to its mode with the reason. Thread-safe.
    static void note_sweep_header(int hmode);
    // heimdall's control-port replies (data receiver, drain): an error just
    // after the client asked for sweep mode is its reason. Thread-safe.
    static void ingest_sweep_reply(const char* data, size_t n);
    // The sweep's plan to heimdall (set_sweep) - on a reconnect. uWS loop only.
    static void send_sweep_plan();
    static void send_sweep_plan_now();   // the plan + the auto gain switch

    // Generic RTL-SDR mode (server_generic_rtlsdr, header bit 12) switched
    // on / off: tells the pages (operating_mode carries generic_rtlsdr).
    // uWS loop only (the data receiver defers it).
    static void on_generic_rtlsdr_changed();

    // The tuner count (active_num_elements) shrank: VFOs on a tuner that is
    // gone move to tuner 0, the pages are told. Not saved - the snapshot
    // keeps the user's tuners for the array they came from. uWS loop only.
    static void clamp_vfo_tuners();

    // Wideband variant: the antenna ring follows the RF (heimdall throws the
    // switches on every retune, from any source). Updates the ring state and,
    // with the WIDEBAND topology, every VFO's array radius; broadcasts the
    // variant state. Called by the FREQ handler and by the data receiver when
    // the stream's RF changes (retunes made by heimdall's UI or the
    // continuous scanner). Thread-safe; returns whether the ring changed.
    // from_stream: the RF came from the packet headers (float on the wire),
    // which keep reporting the OLD RF for a moment after a commanded retune
    static bool follow_wideband_ring(uint64_t rf_hz, bool from_stream = false);

    // 🔊 The VFOs heard in the live audio: exactly `ids` (none = silence) /
    // only `id` (what SET_FM_DECIMATOR does) - their channels in the audio
    // mixer follow, browsers told (decimator_info) - without saving the VFO
    // snapshot (the continuous scanner's switches are temporary). false = no
    // such decimator (nothing changed). Thread-safe.
    static bool set_audio_vfos(const std::vector<int>& ids);
    static bool listen_only(int id);

    // A decoder plugin of VFO `vfo` asks to put rf_hz at the VFO's centre
    // (kp::Host::tune): checked now (mode, tuner range), then FREQ (coherent)
    // or TUNER_FREQ for the VFO's tuner (independent) is dispatched on the
    // uWS loop like a browser's command - saved, sent to heimdall, shown in
    // every page. Returns "" when passed on, else why not. Thread-safe.
    static std::string request_plugin_tune(int vfo, double rf_hz);

private:
    static void handle_message_impl(std::string_view message);
};
