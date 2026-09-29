#pragma once

#include "../core/types.hpp"
#include "../dsp/correlation.hpp"
#include <thread>
#include <string>

// Web server management
void web_server_main(CorrelationResult& correlation_result, FFTProcessingControl& fft_control);

// Global web server state - use void* to avoid uWS header dependencies
extern std::thread web_thread;
extern void* global_app;

// uWebSockets C interface for timer
extern "C" {
    struct us_timer_t* us_create_timer(struct us_loop_t* loop, int fallthrough, unsigned int ext_size);
    void us_timer_set(struct us_timer_t* timer, void (*cb)(struct us_timer_t* t), int ms, int repeat_ms);
}