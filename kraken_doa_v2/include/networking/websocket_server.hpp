#pragma once

#include <string>
#include "App.h"

class WebSocketServer {
public:
    static uWS::SSLApp create_ssl_app();
    static void web_server_main();
    static void doa_http_server_thread();  // Plain HTTP server for DOA_value.html
    static std::string load_html_content();
    static void verify_ssl_certificates();
    static void broadcast_json_message(const std::string& json);
    static bool auth_enabled();   // an API token is configured
};
