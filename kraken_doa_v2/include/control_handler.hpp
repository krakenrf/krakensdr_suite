#pragma once

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

private:
    static void handle_message_impl(std::string_view message);
};
