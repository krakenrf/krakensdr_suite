#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ControlHandler {
public:
    static void handle_websocket_message(std::string_view message);
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
    static std::string build_operating_mode_json();

    // Data receiver: heimdall's mode from the packet header (bits 10/11 of
    // the phase-state field). A mismatch that outlasts a client-initiated
    // change is adopted (heimdall restored its saved mode after a restart,
    // or another control client switched it). Thread-safe.
    static void note_server_mode(int mode);

    // Wideband variant: the antenna ring follows the RF (heimdall throws the
    // switches on every retune, from any source). Updates the ring state and,
    // with the WIDEBAND topology, every VFO's array radius; broadcasts the
    // variant state. Called by the FREQ handler and by the data receiver when
    // the stream's RF changes (retunes made by heimdall's UI or the
    // continuous scanner). Thread-safe; returns whether the ring changed.
    // from_stream: the RF came from the packet headers (float on the wire),
    // which keep reporting the OLD RF for a moment after a commanded retune
    static bool follow_wideband_ring(uint64_t rf_hz, bool from_stream = false);

    // Point the FM demodulator at decimator `id` the way SET_FM_DECIMATOR does
    // (its demod mode applied, audio reset, browsers told) - without saving the
    // VFO snapshot (the continuous scanner's switches are temporary). false =
    // no such decimator. Thread-safe.
    static bool switch_fm_source(int id);

private:
    static void handle_message_impl(std::string_view message);
};
