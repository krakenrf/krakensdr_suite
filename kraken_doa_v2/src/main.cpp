#include "globals.hpp"
#include "config.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "signal_processing/beamformer.hpp"
#include "decimator_manager.hpp"
#include "scanner_manager.hpp"
#include "continuous_scanner.hpp"
#include "networking/websocket_server.hpp"
#include "networking/data_receiver.hpp"
#include "networking/tcp_client.hpp"
#include "networking/gpsd_client.hpp"
#include "networking/web_mapper.hpp"
#include "station_info.hpp"
#include "utils/ring_buffer.hpp"
#include "utils/system_stats.hpp"
#include "utils/status_dashboard.hpp"
#include "channel_manager.hpp"
#include "settings_store.hpp"
#include "doa_logger.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <malloc.h>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

using namespace std;
using namespace chrono;

// Global state definitions
atomic<bool> running{true};
mutex fft_mutex;
vector<vector<float>> fft_magnitudes(MAX_CHANNELS);
vector<vector<float>> fft_averaged(MAX_CHANNELS);
atomic<float> averaging_alpha{0.15f};  // 15% weight on new data (85% averaging on slider)
atomic<int> num_channels{0};
atomic<int> active_num_elements{DOA_DEFAULT_ELEMENTS};  // Synced from the server's packet header once data flows

// KerberosSDR support mode (synced from the packet header's high bits)
atomic<bool> server_kerberos_mode{false};
atomic<bool> server_cal_stale{false};
atomic<int> active_channel{0};

atomic<bool> data_ready{false};
atomic<bool> fm_enabled{true};
atomic<bool> doa_enabled{true};
atomic<bool> beamforming_enabled{false};
atomic<int> ws_client_count{0};  // live browser WebSocket client count (dashboard)

// Beamformed FFT data is per-decimator now (DecimatorInstance::beamformed_fft).

// Wideband scan mode
atomic<bool> wideband_mode_enabled{false};
atomic<bool> doa_enabled_before_wideband{false};  // Store DoA state before wideband
atomic<bool> fm_enabled_before_scanner{false};     // Store FM state before discrete scanner
atomic<uint32_t> fft_reset_generation{0};          // Generation counter for FFT reset (scanner retune)
atomic<uint32_t> wideband_last_reset_gen{0};       // Track wideband FFT reset generation
atomic<bool> fft_data_valid{true};                 // False after reset until first valid FFT frame
array<atomic<uint64_t>, MAX_CHANNELS> tuner_frequencies;

// KrakenSDR Wideband (downconverter) variant - set by --wideband at startup
atomic<bool> wb_variant_enabled{false};
atomic<int> wb_variant_mixer_side{WB_SIDE_HIGH};
atomic<int> wb_variant_array{0};
atomic<int> wb_variant_lo_current{3};
atomic<bool> wb_topology_active{false};
vector<float> wideband_fft_magnitudes;
vector<float> wideband_fft_averaged;
mutex wideband_fft_mutex;

// Wideband reference noise floor for coherent mode normalization
// This is the maximum noise floor from all tuners in wideband mode
// Used to normalize coherent mode FFT to match wideband display levels
atomic<float> wideband_reference_noise_floor{-200.0f};  // Start very low (invalid)

TCPClient data_client, control_client;

moodycamel::ConcurrentQueue<FFTWorkItem> fft_work_queue;
mutex fft_queue_mutex;
condition_variable fft_work_available;

SystemStats global_stats;

FMDemodulatorRobust fm_demod;

ScannerManager scanner_manager;

// GPS/gpsd client: background thread streams location/heading from gpsd into a
// thread-safe snapshot consumed by the API status and DOA_value.html.
GpsdClient gps_client;

// Station identity + location source (callsign, mobile/static/gps). Resolves
// the effective location reported on the API and DOA_value.html.
StationInfo station_info;

// Beamformers are per-decimator now (DecimatorInstance::beamformer), created
// lazily as decimators are added (after FFTW wisdom is loaded at startup).

// FFT management - per-channel contexts for true parallel processing
std::array<ChannelFFTContext, MAX_CHANNELS> channel_fft_contexts;

// Global FFTW mutex - FFTW plan creation/destruction is NOT thread-safe
std::mutex fftw_planner_mutex;

// Dynamic FFT settings
std::atomic<int> current_fft_size{FFT_SIZE};
std::atomic<int> current_fft_decimation{8};  // FFT display downsampling (separate from signal decimation)
std::atomic<float> current_edge_clip{0.8f};  // Edge clip percentage (0.0-1.0, default 80%)

// FFT resize synchronization
// When resizing, we must drain all in-flight FFT operations before destroying contexts
std::atomic<bool> fft_resize_in_progress{false};
std::atomic<int> fft_workers_active{0};
std::mutex fft_resize_mutex;
std::condition_variable fft_resize_complete;

// Squelch settings
// Default squelch level is 15dB above normalized noise floor (0dB)
std::atomic<bool> squelch_enabled{false};
std::atomic<float> squelch_level_db{15.0f};
std::atomic<bool> squelch_open{false};

// Manual steering angle override for beamforming diagnostics
std::atomic<bool> manual_steering_enabled{false};      // When true, use manual angle instead of DoA
std::atomic<float> manual_steering_angle{0.0f};        // Manual steering angle in degrees (0-360)

// Persistent data buffer (stores complex<float> to avoid wasteful conversions)
vector<complex<float>> persistent_data_buffer;
size_t persistent_buffer_size = 0;
mutex persistent_buffer_mutex;
atomic<uint32_t> persistent_buffer_generation{1};

// uWS globals
uWS::Loop* loop = nullptr;
struct us_timer_t* broadcast_timer = nullptr;
struct us_timer_t* audio_timer = nullptr;
struct us_timer_t* doa_timer = nullptr;
uWS::SSLApp* global_ssl_app = nullptr;

void initialize_persistent_buffer() {
    lock_guard<mutex> lock(persistent_buffer_mutex);
    // Allocate 16 MB worth of complex<float> samples (each is 8 bytes)
    persistent_data_buffer.resize(16 * 1024 * 1024 / sizeof(std::complex<float>));
}

// SIGINT/SIGTERM/SIGHUP → clear `running`; worker loops poll it (the data receiver's
// recv has a 500 ms timeout for exactly this) and main's joins then complete
static void handle_shutdown_signal(int) {
    running.store(false);
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    // Pre-connect default only: once data flows, the element count follows
    // the server's packet header (see data_receiver.cpp).
    int num_elements = DOA_DEFAULT_ELEMENTS;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--num-elements") == 0) && i + 1 < argc) {
            num_elements = atoi(argv[++i]);
            if (num_elements < 2 || num_elements > DOA_NUM_ELEMENTS) {
                cerr << "Error: -n must be between 2 and " << DOA_NUM_ELEMENTS << endl;
                return 1;
            }
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wideband") == 0) {
            wb_variant_enabled = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            cout << "Usage: " << argv[0] << " [options]\n"
                 << "Options:\n"
                 << "  -n, --num-elements <N>  Pre-connect element count (2-" << DOA_NUM_ELEMENTS << ", default: " << DOA_DEFAULT_ELEMENTS << ");\n"
                 << "                          overridden by the server's channel count once data arrives\n"
                 << "  -w, --wideband          KrakenSDR Wideband variant: web UI gains RF tuning across the\n"
                 << "                          downconverter range plus high/low/below mixing-side control\n"
                 << "                          (IF " << WB_VARIANT_IF_HZ / 1e6 << " MHz; start heimdall with --wideband too)\n"
                 << "  -h, --help              Show this help message\n";
            return 0;
        }
    }

    // Errors-only file logging: when stdout is NOT a terminal (output captured
    // to a logfile / journal), discard cout entirely so a deployment that runs
    // for years accumulates only stderr (errors) in its log - no routine
    // status dumps or connection chatter. Interactive runs (TTY) are
    // unaffected, and KRAKEN_DOA_VERBOSE_LOG=1 restores full output to files
    // for debugging.
    if (!isatty(STDOUT_FILENO) && !getenv("KRAKEN_DOA_VERBOSE_LOG")) {
        static struct : streambuf {
            int overflow(int c) override { return traits_type::not_eof(c); }
            streamsize xsputn(const char*, streamsize n) override { return n; }
        } null_buf;
        cout.rdbuf(&null_buf);
    }

    // Store in global for use by other modules (MUSIC processor, beamformer, UI)
    active_num_elements.store(num_elements);

    // Ctrl+C / kill must trigger the orderly shutdown below instead of
    // hard-killing the process
    signal(SIGINT, handle_shutdown_signal);
    signal(SIGTERM, handle_shutdown_signal);
    // tmux kill-session (run.sh stop / systemd stop) and a closed terminal
    // send SIGHUP; without a handler it killed the process mid-write
    signal(SIGHUP, handle_shutdown_signal);

    // FIX: Increase malloc arena count to reduce contention with many threads
    // With 2 decimators × 5 channels = 10 threads, default arenas (8) cause lock contention
    mallopt(M_ARENA_MAX, 32);  // Allow up to 32 malloc arenas

    // Take over the terminal for the live dashboard BEFORE the heavy init below,
    // so every startup message (FFTW wisdom, channels, server connect, ...) is
    // captured and shown live on the "startup" screen — i.e. WHAT is loading —
    // instead of a static "Loading...". No-op on a non-TTY (piped/redirected),
    // where the plain scrolling text path is used instead.
    StatusDashboard::begin();

    cout << "KrakenSDR DoA for Heimdall - Raw Data Buffering Mode" << endl;
    cout << "=======================================================================" << endl;
    cout << "Active Elements: " << num_elements << " (of " << DOA_NUM_ELEMENTS << " max)" << endl;
    cout << "Web UI: https://krakensdr:" << WEB_PORT << " (HTTPS required)" << endl;
    if (wb_variant_enabled.load()) {
        cout << "Mode: KrakenSDR WIDEBAND variant (IF " << WB_VARIANT_IF_HZ / 1e6
             << " MHz, high/low mixing-side control in web UI)" << endl;
    }
    cout << "=======================================================================" << endl;

    // Initialize components
    FFTProcessor::initialize();  // Loads FFTW wisdom file

    // Beamformers are created per-decimator (DecimatorManager::initialize, run
    // from the data receiver thread). FFTW wisdom is already loaded here, so the
    // FFTW_MEASURE plans built in each Beamformer's constructor stay fast.

    ChannelManager::initialize();

    // Start the gpsd client (reads location/heading in the background; harmless
    // no-op if gpsd is down or no receiver is attached).
    gps_client.start();

    // Decimator manager initialized in data_receiver_thread

    initialize_persistent_buffer();

    // Load the settings file (created with all defaults on first run), then
    // start the debounced writer. The settings are replayed by the data
    // receiver once it connects (decimators exist, server reachable).
    SettingsStore::load();
    SettingsStore::start();

    // Web mapper output worker (idle until enabled; config arrives with the
    // settings replay / from the web UI sidebar).
    web_mapper.start();

    // Ensure the recordings folder exists so the UI can list/download from it.
    cout << "DoA recordings folder: " << doa_recordings_dir() << endl;

    // Start worker threads
    thread data_worker(DataReceiver::data_receiver_thread);
    thread decimation_worker(DataReceiver::decimation_processor_thread);

    // Create multiple FFT processor threads for parallel processing
    // This is critical for wideband mode where all channels need processing
    const int NUM_FFT_THREADS = 8;  // One per channel for true parallelism
    vector<thread> fft_workers;
    for (int i = 0; i < NUM_FFT_THREADS; i++) {
        fft_workers.emplace_back(DataReceiver::fft_processor_thread);
    }
    cout << "Started " << NUM_FFT_THREADS << " FFT processor threads for parallel channel processing" << endl;

    thread fm_worker(DataReceiver::fm_processor_thread);
    thread web_worker(WebSocketServer::web_server_main);
    thread doa_http_worker(WebSocketServer::doa_http_server_thread);
    
    // Status monitoring thread. On an interactive terminal this drives the live
    // in-place dashboard (StatusDashboard); otherwise it falls back to the 30s
    // scrolling text dump. With output redirected to a file the dump lands on
    // the discarded cout (errors-only logging, see main) unless
    // KRAKEN_DOA_VERBOSE_LOG=1 re-enables it.
    thread status_worker([&]() {
        // begin() was already called on the main thread (before init) so startup
        // logs are captured; it is idempotent, so this thread just renders.
        const bool tui = StatusDashboard::active();

        while (running) {
            // Heimdall answers every control command and broadcasts status at
            // 2 Hz; nothing in this process consumes either. Leaving the socket
            // unread fills its receive buffer and back-pressures heimdall's
            // control server until it stalls on a blocking send, killing
            // frequency/gain control (see DataReceiver::drain_control_socket).
            DataReceiver::drain_control_socket();

            if (tui) {
                StatusDashboard::render();
                // Fast refresh for a live feel; poll `running` between slices.
                for (int i = 0; i < 4 && running; i++)
                    this_thread::sleep_for(milliseconds(250));
                continue;
            }

            // ---- legacy plain-text status (non-TTY / opted out) ----
            for (int i = 0; i < 30 && running; i++) {
                this_thread::sleep_for(seconds(1));
                DataReceiver::drain_control_socket();
            }
            if (!running) break;

            int current_active = active_channel.load();

            cout << "\n=== RAW DATA BUFFER STATUS ===" << endl;
            DataReceiver::print_raw_buffer_status();

            if (fm_enabled.load() && fm_demod.is_processing_enabled()) {
                size_t writes, reads, over, under;
                fm_demod.get_buffer_stats(writes, reads, over, under);

                float active_freq_mhz = ChannelManager::get_frequency(current_active) / 1e6f;
                float active_gain_db = ChannelManager::get_gain(current_active);

                cout << "\n=== FM SYSTEM STATUS ===" << endl;
                cout << "Active_Ch=" << current_active << ", "
                     << "Freq=" << active_freq_mhz << "MHz, "
                     << "Gain=" << (active_gain_db < 0 ? "Auto" : to_string(active_gain_db) + "dB") << ", "
                     << "FM_fill=" << fm_demod.get_buffer_fill() << "%, "
                     << "Overruns=" << over << ", Signal=" << fm_demod.get_signal_strength() << endl;
            }

            if (doa_enabled.load()) {
                cout << "\n=== DOA SYSTEM STATUS ===" << endl;
                cout << "Decimators: " << decimator_manager.getDecimatorCount() << endl;
            }

            cout << "\n=== THREAD STATISTICS ===" << endl;
            global_stats.print_summary();
        }

        StatusDashboard::end();  // restore terminal + flush captured log tail
    });
    
    // Block until shutdown: the data receiver exits when `running` is
    // cleared by the signal handler (its recv has a 500 ms timeout), or when
    // it terminates on its own.
    if (data_worker.joinable()) data_worker.join();

    running = false;
    cout << "\nShutting down..." << endl;

    // Stop the continuous scanner first: its thread drives decimators, the
    // FFT processor and the websocket broadcast path, all torn down below.
    // stop() joins the scanner thread and is a no-op if it never ran.
    continuous_scanner.stop();

    // Stop the gpsd client thread (its socket read has a 2s timeout so the
    // join completes promptly).
    gps_client.stop();

    // Stop the DoA logger: flush + close the open file before the decimators
    // and station/scanner state it reads are torn down.
    doa_logger.stop();

    // Stop the web mapper worker (reads the same decimator/station state).
    web_mapper.stop();

    // Signal all condition variables for shutdown
    {
        lock_guard<mutex> lock(fft_queue_mutex);
        fft_work_available.notify_all();
    }

    // Join the processing threads (all poll `running`)
    if (decimation_worker.joinable()) decimation_worker.join();
    if (fm_worker.joinable()) fm_worker.join();

    for (auto& fft_thread : fft_workers) {
        if (fft_thread.joinable()) fft_thread.join();
    }

    if (status_worker.joinable()) status_worker.join();

    // The uWS event loops (web UI + DoA HTTP) block in run() with no clean
    // stop mechanism - detach them and exit without running static
    // destructors, which those threads could otherwise race against.
    web_worker.detach();
    doa_http_worker.detach();

    SettingsStore::stop();    // Joins the writer thread, flushing any last change

    FFTProcessor::cleanup();  // Saves FFTW wisdom

    cout << "Shutdown complete" << endl;
    cout.flush();
    _Exit(0);
}