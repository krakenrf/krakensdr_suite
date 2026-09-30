#include "core/types.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "core/settings.hpp"
#include "core/forward_comp.hpp"
#include "core/utils.hpp"
#include "sdr/sdr_init.hpp"
#include "sdr/sdr_pipeline.hpp"
#include "sdr/sdr_device.hpp"
#include "sdr/pipeline_control.hpp"
#include "sdr/downconverter.hpp"
#include "sdr/kerberos_gpio.hpp"
#include "dsp/correlation.hpp"
#include "dsp/compensation.hpp"
#include "net/tcp_data_server.hpp"
#include "net/tcp_control_server.hpp"
#include "net/rtl_tcp_server.hpp"
#include "web/web_server.hpp"
#include "web/html_loader.hpp"
#include "core/status_dashboard.hpp"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <csignal>
#include <thread>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>

// Global state variables (definitions for extern declarations in types.hpp)
std::atomic<uint64_t> current_frequency{CENTER_FREQ};
std::atomic<int> current_gain{AUTO_GAIN_MODE ? -1 : GAIN};
std::atomic<bool> bias_tee_enabled{true};
std::atomic<uint32_t> antenna_bias_tee_mask{0};
std::atomic<int> rtl_tcp_channel{0};
std::atomic<bool> global_running{true};
std::atomic<OperatingMode> operating_mode{OperatingMode::COHERENT};
std::atomic<int> active_num_elements{NUM_DEVICES};  // Resolved in main(): -n flag > persisted setting > devices detected on USB

// Channel N = the device with USB serial expected_serials[N]. Overridden by
// --serials during argument parsing (before any thread starts); immutable after.
// The default list covers the full NUM_DEVICES ceiling so the element count can
// be raised up to 8 without --serials; a stock KrakenSDR only has 1000-1004
// attached, and the startup default counts what is actually present.
std::vector<std::string> expected_serials = {"1000", "1001", "1002", "1003",
                                             "1004", "1005", "1006", "1007"};
// KerberosSDR support (--kerberos): the older 4-channel hardware has no
// noise-source RF switch - the noise is coupled in through a directional
// coupler, so the antennas must be MANUALLY disconnected for a calibration to
// be valid. In this mode nothing ever turns the noise source on automatically:
// the system starts UNCALIBRATED, calibration runs only from the explicit
// recalibrate command (user confirms antennas are disconnected), a retune or
// gain change marks the calibration STALE instead of recalibrating, and a
// coherence loss flushes and drops back to UNCALIBRATED.
std::atomic<bool> kerberos_mode{false};
// --kerberos_sw: KerberosSDR modified with CKOVAL antenna switches on Pi
// GPIOs; implies kerberos_mode but restores automatic calibration (Phase 4).
std::atomic<bool> kerberos_sw_mode{false};
// Calibration completed at some frequency, but settings changed since (only
// meaningful in kerberos mode; cleared when a calibration converges).
std::atomic<bool> kerberos_cal_stale{false};

std::atomic<bool> coherence_lost{false};
std::atomic<bool> recovery_in_progress{false};
std::atomic<uint32_t> coherence_event_count{0};
std::atomic<bool> force_recalibration{false};
std::atomic<bool> periodic_recal_enabled{PERIODIC_RECAL_DEFAULT_ENABLED != 0};
std::atomic<int> periodic_recal_minutes{PERIODIC_RECAL_DEFAULT_MINUTES};
std::atomic<uint32_t> periodic_recal_lag_fail_count{0};
std::atomic<uint32_t> periodic_recal_phase_fail_count{0};
WidebandConfig wideband_config;
DiscreteScannerConfig discrete_scanner;

// Global variables
std::vector<std::unique_ptr<SDRDevice>> devices;
std::optional<PhaseCompensationData> phase_compensation = std::make_optional<PhaseCompensationData>();
std::mutex settings_mutex;
CorrelationResult correlation_result;
FFTProcessingControl fft_control;
PerBinCalibration per_bin_cal;

// TCP server instances
std::unique_ptr<TcpDataServer> tcp_data_server;
std::unique_ptr<TcpControlServer> tcp_control_server;
std::unique_ptr<RtlTcpServer> rtl_tcp_server;

// Signal handler: only async-signal-safe work - record the signal and clear
// the (lock-free) running flag. main() polls the flag, logs, and runs the
// orderly shutdown. (Closing the uWS timer / waking the loop here was unsafe:
// it frees memory and touches epoll state the web thread may be using, and
// main never needed the loop to exit - it _Exits after releasing the dongles.)
static volatile std::sig_atomic_t shutdown_signal = 0;
void signal_handler(int sig) {
    shutdown_signal = sig;
    global_running = false;
}

// TCP Status broadcaster
void tcp_status_broadcaster() {
    std::cout << "Starting TCP status broadcaster" << std::endl;

    while (global_running) {
        if (tcp_control_server) {
            tcp_control_server->broadcast_status();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 2 Hz status updates
    }
}

// Discrete scanner thread - handles server-side frequency switching
void discrete_scanner_thread() {
    std::cout << "Discrete scanner thread started (inactive until enabled)" << std::endl;

    while (global_running) {
        // Check if scanner is enabled
        if (!discrete_scanner.enabled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Get scanner configuration
        size_t num_groups = discrete_scanner.get_num_groups();
        if (num_groups == 0) {
            // No frequency groups configured, disable scanner
            discrete_scanner.enabled = false;
            std::cout << "Scanner disabled: no frequency groups configured" << std::endl;
            continue;
        }

        uint32_t dwell_time = discrete_scanner.dwell_time_ms.load();
        uint32_t settling_time = discrete_scanner.settling_time_ms.load();
        uint32_t current_index = discrete_scanner.current_group_index.load();

        // Get next frequency (index wrapped into the current list)
        uint64_t next_frequency = discrete_scanner.get_frequency(current_index, &current_index);
        if (next_frequency == 0) continue;  // list cleared meantime: re-checked above

        // STEP 1: Set retuning flag BEFORE changing frequency
        // This tells client to discard incoming data during settling
        discrete_scanner.retuning_in_progress.store(true, std::memory_order_release);

        // STEP 2: Flush all sample buffers BEFORE changing frequency
        // This removes any stale data that was captured at the old frequency
        // Critical: Without this, packets already in L1/L2 buffers get sent with
        // retuning_in_progress=true but contain old frequency data
        clear_l1_buffer();
        clear_l2_buffer();

        // Change frequency on all devices
        std::cout << "Scanner: Tuning to group " << current_index << ": "
                  << (next_frequency / 1e6) << " MHz (settling " << settling_time << "ms)" << std::endl;

        {
            std::lock_guard<std::mutex> lock(settings_mutex);
            bool hop_ok = true;

            // If wideband mode is enabled, update wideband tuner spread around this center
            if (wideband_config.enabled.load()) {
                setup_wideband_frequencies(next_frequency, devices);
            } else if (downconverter.enabled.load()) {
                // Wideband variant: tuners stay at the IF, hop the LO instead
                // (auto-switches mixer side / antenna ring if the hop needs it)
                hop_ok = wideband_retune_rf(next_frequency, devices);
                if (!hop_ok) std::cerr << "Scanner: LO hop to " << next_frequency / 1e6
                                       << " MHz failed - still on " << current_frequency.load() / 1e6 << " MHz" << std::endl;
            } else {
                // Coherent mode: set all tuners to same frequency. A tuner that
                // didn't take it (it used to be ignored) gets one retry; one
                // still off frequency is logged - the array is split until the
                // next hop.
                std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
                for (const auto& device : devices) {
                    if (!device || !device->dev) continue;
                    if (rtlsdr_set_center_freq(device->dev, static_cast<uint32_t>(next_frequency)) < 0 &&
                        rtlsdr_set_center_freq(device->dev, static_cast<uint32_t>(next_frequency)) < 0) {
                        std::cerr << "Scanner: channel " << device->index << " failed to tune to "
                                  << next_frequency / 1e6 << " MHz" << std::endl;
                    }
                }
            }
            if (hop_ok) {
                current_frequency = next_frequency;
                // The S2P forward correction is frequency dependent: follow the
                // hop (it stayed at the pre-scan frequency's value)
                if (forward_comp.enabled.load(std::memory_order_relaxed)) {
                    fwdcomp::recompute(static_cast<double>(next_frequency));
                }
            }
        }

        // Increment frequency change counter (this signals the client that frequency changed)
        discrete_scanner.frequency_change_counter.fetch_add(1, std::memory_order_release);

        // STEP 3: Wait for tuners to settle (RTL-SDR produces noise during retuning)
        std::this_thread::sleep_for(std::chrono::milliseconds(settling_time));

        // STEP 4: Flush buffers AGAIN after settling to remove noisy settling data
        // The tuners produce garbage during the settling period
        clear_l1_buffer();
        clear_l2_buffer();

        // STEP 5: Clear retuning flag - tuners are now stable with clean data
        // Client will reset FFT and start accepting clean data
        discrete_scanner.retuning_in_progress.store(false, std::memory_order_release);

        // Dwell on this frequency (subtract settling time from total dwell)
        uint32_t remaining_dwell = (dwell_time > settling_time) ? (dwell_time - settling_time) : 0;
        if (remaining_dwell > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining_dwell));
        }

        // Move to next frequency group (wrap around; get_frequency re-wraps
        // if the list was replaced during the dwell)
        uint32_t next_index = (current_index + 1) % std::max<size_t>(discrete_scanner.get_num_groups(), 1);
        discrete_scanner.current_group_index = next_index;
    }

    std::cout << "Discrete scanner thread exiting" << std::endl;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    int num_elements_cli = 0;  // 0 = not given on the command line

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--num-elements") == 0) && i + 1 < argc) {
            num_elements_cli = atoi(argv[++i]);
            if (num_elements_cli < 2 || num_elements_cli > NUM_DEVICES) {
                std::cerr << "Error: -n must be between 2 and " << NUM_DEVICES << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "--serials") == 0 && i + 1 < argc) {
            // Comma-separated USB serials, channel order. Replaces the default
            // KrakenSDR list so any set of dongles can form the array.
            expected_serials.clear();
            std::string list = argv[++i];
            size_t start = 0;
            while (start <= list.size()) {
                size_t comma = list.find(',', start);
                if (comma == std::string::npos) comma = list.size();
                std::string serial = list.substr(start, comma - start);
                if (!serial.empty()) expected_serials.push_back(serial);
                start = comma + 1;
            }
            if (expected_serials.size() < 2 || expected_serials.size() > NUM_DEVICES) {
                std::cerr << "Error: --serials needs 2-" << NUM_DEVICES
                          << " comma-separated serials (got " << expected_serials.size() << ")" << std::endl;
                return 1;
            }
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wideband") == 0) {
            downconverter.enabled = true;
        } else if (strcmp(argv[i], "--kerberos") == 0) {
            kerberos_mode = true;
        } else if (strcmp(argv[i], "--kerberos_sw") == 0) {
            kerberos_mode = true;
            kerberos_sw_mode = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -n, --num-elements <N>  Number of antenna elements to use (2 up to the\n"
                      << "                          serial-list length; default: all listed serials,\n"
                      << "                          or the value last set in the web UI)\n"
                      << "  --serials <s0,s1,...>   USB serials in channel order (default 1000,...,1007;\n"
                      << "                          up to " << NUM_DEVICES << " entries)\n"
                      << "  -w, --wideband          KrakenSDR Wideband variant: tuners parked at the "
                      << WB_VARIANT_IF_HZ / 1e6 << " MHz IF,\n"
                      << "                          retunes reprogram the built-in downconverter LO (moRFeus)\n"
                      << "  --kerberos              KerberosSDR: no noise-source RF switch (noise coupled\n"
                      << "                          via directional coupler). Manual calibration only -\n"
                      << "                          disconnect the antennas, then press Recalibrate in the\n"
                      << "                          web UI. No automatic recalibration ever runs\n"
                      << "  --kerberos_sw           KerberosSDR with CKOVAL antenna switches on Pi GPIOs:\n"
                      << "                          like --kerberos but calibration is automatic again\n"
                      << "  -h, --help              Show this help message\n";
            return 0;
        }
    }

    // --kerberos_sw needs the CKOVAL switch GPIOs before any calibration can
    // be allowed to run automatically. If they are unavailable (not a
    // Raspberry Pi, gpiochip inaccessible, pins claimed), fall back to plain
    // --kerberos: automatic calibration without antenna switching would
    // calibrate against live antenna signals.
    if (kerberos_sw_mode.load() && !kerberos_gpio_init()) {
        std::cerr << "--kerberos_sw: antenna switch GPIOs unavailable - falling back to"
                     " MANUAL calibration mode (--kerberos)" << std::endl;
        kerberos_sw_mode = false;
    }

    const int max_elements = static_cast<int>(expected_serials.size());
    if (num_elements_cli > max_elements) {
        std::cerr << "Error: -n " << num_elements_cli << " exceeds the " << max_elements
                  << "-entry serial list" << std::endl;
        return 1;
    }

    // Errors-only file logging: when stdout is NOT a terminal (output captured
    // to a logfile / journal), discard cout entirely so a deployment that runs
    // for years accumulates only stderr (errors) in its log - no routine
    // status, calibration or connection chatter. Interactive runs (TTY) are
    // unaffected, and HEIMDALL_VERBOSE_LOG=1 restores full output to files
    // for debugging.
    if (!isatty(STDOUT_FILENO) && !std::getenv("HEIMDALL_VERBOSE_LOG")) {
        static struct : std::streambuf {
            int overflow(int c) override { return traits_type::not_eof(c); }
            std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
        } null_buf;
        std::cout.rdbuf(&null_buf);
    }

    // Load persisted runtime settings (e.g. per-bin EQ toggle, web-UI element
    // count) before any calibration starts, so a remembered "on" builds the
    // equalizer on the very first calibration.
    settings::load();

    // Resolve the startup element count: explicit -n wins, then the web-UI
    // choice persisted in the settings file, then however many of the expected
    // serials are actually attached (the default serial list covers the full
    // 8-channel ceiling, but a stock KrakenSDR has 5 dongles and a KerberosSDR
    // 4). Persisted values outside the current serial list (e.g. after a
    // shorter --serials) fall back rather than error.
    // KerberosSDR is 4-channel hardware: --kerberos pins the default to 4
    // (ignoring the persisted value and USB auto-detection, which could see 5
    // when simulating on a KrakenSDR); only an explicit -n overrides it.
    int num_elements = num_elements_cli;
    if (num_elements == 0 && kerberos_mode.load()) {
        num_elements = std::min(4, max_elements);
        std::cout << "KerberosSDR mode: defaulting to " << num_elements
                  << " elements (override with -n)" << std::endl;
    }
    if (num_elements == 0) {
        const int persisted = settings::persisted_num_elements.load(std::memory_order_acquire);
        if (persisted >= 2 && persisted <= max_elements) {
            num_elements = persisted;
        } else {
            const int present = count_expected_devices_present();
            num_elements = std::clamp(present, 2, max_elements);
            std::cout << "Auto-detected " << present << " of the " << max_elements
                      << " expected devices on USB, starting with "
                      << num_elements << " elements" << std::endl;
        }
    }
    active_num_elements.store(num_elements);

    std::cout << "Heimdall Standalone - C++20 with uWebSockets + RAII FFT + TCP Multi-Channel Streaming + RTL-TCP (ENHANCED)\n"
         << "=========================================================================================================\n"
         << "Enhanced with TCP servers for FFT viewer integration and RTL-TCP compatible output\n"
         << "Modified for serial number-based RTL-SDR initialization\n"
         << "ENHANCED: RTL-TCP server with selectable source channel via web interface\n"
         << "MODULAR: Split into core, sdr, dsp, net, and web modules\n"
         << "Active Elements: " << num_elements << " (of " << max_elements << " configured), Reference: " << REF_CHANNEL
         << ", Samples: " << NUM_SAMPLES << "\n"
         << "Frequency: " << std::fixed << std::setprecision(1) << CENTER_FREQ/1e6
         << " MHz, Sample Rate: " << SAMPLE_RATE/1e6 << " MSPS\n"
         << (downconverter.enabled.load()
                 ? std::string("Mode: KrakenSDR WIDEBAND variant (tuners at ") +
                       std::to_string(static_cast<unsigned>(WB_VARIANT_IF_HZ / 1000000)) +
                       " MHz IF, downconverter LO retuning)\n"
                 : std::string())
         << (kerberos_mode.load()
                 ? std::string("Mode: KerberosSDR (") +
                       (kerberos_sw_mode.load()
                            ? "CKOVAL antenna switches - automatic calibration)\n"
                            : "MANUAL calibration only - disconnect antennas, then Recalibrate)\n")
                 : std::string())
         << "Web Server: http://localhost:" << WEB_PORT << " (uWebSockets)\n"
         << "RTL-TCP Port: " << RTL_TCP_PORT << " (Selectable channel via web UI)\n"
         << "TCP Data Port: " << TCP_DATA_PORT << " (Multi-channel IQ streaming)\n"
         << "TCP Control Port: " << TCP_CONTROL_PORT << " (JSON commands)\n"
         << "=========================================================================================================\n" << std::endl;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    // SIGHUP too: tmux kill-session (run.sh stop, the systemd unit's stop)
    // and a closed terminal deliver it - without a handler it killed heimdall
    // outright, skipping the dongle release and the KerberosSDR GPIO restore.
    signal(SIGHUP, signal_handler);

    // Live status dashboard. Take over the terminal BEFORE the (slow) device
    // init below so its progress shows on the loading screen; a background
    // thread renders it. The RAII guard restores the terminal on EVERY exit
    // path, including the early error returns below. No-op on a non-TTY, where
    // the plain scrolling logs are used instead.
    StatusDashboard::begin();
    struct DashboardGuard {
        std::thread th;
        bool stopped = false;
        void stop() {
            if (stopped) return;
            stopped = true;
            global_running = false;
            if (th.joinable()) th.join();
            StatusDashboard::end();  // restore terminal (idempotent)
        }
        ~DashboardGuard() { stop(); }
    } dash;
    if (StatusDashboard::active()) {
        dash.th = std::thread([]() {
            while (global_running) {
                StatusDashboard::render();
                for (int i = 0; i < 4 && global_running; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
    }

    std::string html_content;
    if (!load_html_file(html_content)) {
        std::cerr << "Failed to load index.html" << std::endl;
        return 1;
    }
    
    if (!init_all_rtlsdr_devices(devices)) {
        std::cerr << "Device initialization failed" << std::endl;
        return 1;
    }

    // KrakenSDR Wideband variant: the tuners were just parked at the IF; bring
    // up the downconverter LO for the startup RF before any data consumers
    // connect. Without its LO the wideband unit produces nothing useful, so a
    // missing moRFeus is fatal in this mode.
    if (downconverter.enabled.load()) {
        if (!downconverter_init(current_frequency.load())) {
            std::cerr << "Downconverter initialization failed - is the moRFeus connected"
                         " (and /dev/hidraw* accessible)?" << std::endl;
            return 1;
        }
    }
    
    // Initialize correlation result with domain-specific setup
    initialize_correlation_result(correlation_result);
    
    // Start RTL-TCP server with default channel 0
    rtl_tcp_server = std::make_unique<RtlTcpServer>(0);
    if (!rtl_tcp_server->start()) {
        std::cerr << "Failed to start RTL-TCP server on port " << RTL_TCP_PORT << std::endl;
        return 1;
    }
    
    // Start TCP servers
    tcp_data_server = std::make_unique<TcpDataServer>();
    tcp_control_server = std::make_unique<TcpControlServer>();
    
    if (!tcp_data_server->start()) {
        std::cerr << "Failed to start TCP data server on port " << TCP_DATA_PORT << std::endl;
        return 1;
    }
    
    if (!tcp_control_server->start()) {
        std::cerr << "Failed to start TCP control server on port " << TCP_CONTROL_PORT << std::endl;
        return 1;
    }
    
    // Set up RTL-TCP server reference in control server
    tcp_control_server->set_rtl_tcp_server(rtl_tcp_server.get());
    
    std::cout << "\nStarting all processing threads..." << std::endl;

    web_thread = std::thread([&](){ web_server_main(correlation_result, fft_control); });
    std::thread correlation_processor_thread([](){ correlation_processor(correlation_result, fft_control); });
    std::thread tcp_status_thread(tcp_status_broadcaster);
    std::thread scanner_thread(discrete_scanner_thread);
    std::thread coherence_watchdog_thread(coherence_watchdog);
    std::thread periodic_recal_thread([](){ periodic_calibration_monitor(correlation_result); });

    // --kerberos: start in the uncalibrated idle state instead of letting the
    // startup calibration run (with the antennas connected it would calibrate
    // against live signals). Data streams with identity compensation; the
    // user disconnects the antennas and presses Recalibrate in the web UI.
    if (kerberos_manual_cal_only()) {
        kerberos_enter_uncalibrated("startup");
    }

    // Device-touching threads (USB readers, drain, conversion, per-channel lag
    // compensation) are managed by pipeline_control so the runtime element-count
    // reconfiguration can stop and restart them without a process restart.
    start_pipeline_threads();

    std::cout << "System running - press Ctrl+C to stop" << std::endl;
    std::cout << "RTL-TCP clients can connect to localhost:" << RTL_TCP_PORT << " (Channel selectable via web interface)" << std::endl;
    std::cout << "Compatible with SDR#, SDR++, GQRX, CubicSDR, etc." << std::endl;
    std::cout << "FFT Viewer can connect to localhost:" << TCP_DATA_PORT << " (data) and localhost:" << TCP_CONTROL_PORT << " (control)" << std::endl;
    std::cout << "Use the web interface to select which RTL-SDR channel to stream via RTL-TCP" << std::endl;

    // Everything is up (devices open, servers running, calibration starting):
    // switch the dashboard from the loading screen to the full status view.
    StatusDashboard::mark_ready();

    while (global_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Restore the terminal (stop + join the dashboard thread) before the
    // shutdown logs so they print normally on the restored screen.
    dash.stop();

    if (shutdown_signal) std::cout << "\nReceived signal " << shutdown_signal << ", shutting down..." << std::endl;
    else std::cout << "\nShutting down..." << std::endl;
    global_running = false;  // signal every worker loop to stop

    // Bulletproof shutdown: DO NOT join the worker threads. The uWebSockets
    // event loop never returns from run(), and other workers (and the TCP
    // server stop() calls) can block on their queues/condition variables, so
    // ANY join here risks hanging shutdown (which it did). Instead we release
    // just the RTL-SDR hardware and _Exit; the OS reclaims the threads, the
    // listening sockets (TCP ports) and all remaining handles on exit.
    //
    // Hardware release: cancel the async reads first (stops USB streaming and
    // returns each reader thread out of rtlsdr_read_async), let any in-flight
    // libusb completion callback drain, then close the dongles so they are
    // clean for the next run. The workers only touch device QUEUES, never the
    // librtlsdr handle, so closing here can't race them.
    for (auto& device : devices) {
        if (device) {
            device->running = false;
            if (device->dev) rtlsdr_cancel_async(device->dev);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
        // A web/control command or a noise-source switch still in flight must
        // not use a handle mid-close.
        std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
        for (auto& device : devices) {
            if (device && device->dev) {
                rtlsdr_close(device->dev);
                device->dev = nullptr;
            }
        }
    }

    // --kerberos_sw: leave the antenna switches pointing at the antennas (not
    // the noise source) and release the GPIO lines for the next run.
    kerberos_gpio_cleanup();

    // _Exit skips static/global destructors that the still-live worker threads
    // could otherwise race against during teardown.
    std::cout << "Heimdall shutdown complete." << std::endl;
    std::cout.flush();
    _Exit(0);
}