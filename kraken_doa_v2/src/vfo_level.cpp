// 📏 Each VFO's received level, in every operating mode - see vfo_level.hpp.
#include "vfo_level.hpp"

#include "channel_manager.hpp"
#include "decimator_manager.hpp"
#include "doa_logger.hpp"      // doa_is_calibrating
#include "globals.hpp"         // active_channel, multi_tuner_mode
#include "signal_processing/fft_processor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

namespace vlvl {

void Level::add(int64_t now, double s, double nz, double tot, float g) {
    if (!t || now - t > GAP_MS || g != gain) {
        sig = s;
        n = 1;
    } else {
        n++;
        sig += std::max(1.0 / n, EMA) * (s - sig);
    }
    noise = nz;
    total = tot;
    gain = g;
    t = now;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {

struct Vfo {
    Level lvl;
    Moment mo;
    double freq = 0;     // RF of the VFO's centre (a retune starts afresh)
};

std::mutex mu;
std::map<int, Vfo> vfos;
std::atomic<bool> g_stop{false};
std::thread thr;

void sample() {
    const int64_t t = now_ms();
    const bool multi = multi_tuner_mode();
    // the noise source fills every band while heimdall calibrates
    const bool ok = !doa_is_calibrating();
    // (this thread is the only writer: the work on a copy, swapped in under the lock)
    std::map<int, Vfo> prev, seen;
    {
        std::lock_guard<std::mutex> lk(mu);
        prev = vfos;
    }
    {
        for (const auto& dec : decimator_manager.getAllDecimators()) {
            if (!dec || !dec->decimator || dec->being_deleted.load()) continue;
            // its tuner: coherent = the FFT channel (all tuners on one band),
            // independent / wideband scan = its own
            const int ch = multi ? dec->tuner_channel.load() : active_channel.load();
            const float off = dec->frequency_offset_hz.load();
            const double freq = ChannelManager::get_frequency(ch) + off;
            auto it = prev.find(dec->id);
            Vfo v = it != prev.end() ? it->second : Vfo{};
            if (v.freq != 0 && std::fabs(freq - v.freq) > RETUNE_HZ) v.lvl = Level{};
            v.freq = freq;
            FFTProcessor::BandPower bp;
            // no DC bins left out (as the FFT squelch): the DC offset is tracked
            // out before the FFT, and a carrier tuned onto the centre (a CW
            // beacon, AM) is the signal itself
            if (ok && (!dec->squelch_enabled.load() || dec->squelch_open.load()) &&
                FFTProcessor::band_power(ch, off, dec->decimator->getBandwidthMhz() * 1e6f, 0, &bp) &&
                bp.sig > bp.thresh) {
                const float gain = ChannelManager::get_gain(ch);
                v.lvl.add(t, bp.sig, bp.noise, bp.total, gain);
                v.mo = Moment{bp.sig, bp.noise, bp.total, gain, t, v.mo.seq + 1,
                              dec->stream_pos.load(std::memory_order_relaxed), dec->decimator->getOutputRate()};
            }
            seen[dec->id] = v;
        }
    }
    std::lock_guard<std::mutex> lk(mu);
    vfos.swap(seen);   // (VFOs gone: dropped)
}

}  // namespace

void start() {
    if (thr.joinable()) return;
    g_stop = false;
    thr = std::thread([] {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SAMPLE_MS));
            sample();
        }
    });
}

void stop() {
    g_stop = true;
    if (thr.joinable()) thr.join();
}

bool level(int vfo, Level* out) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = vfos.find(vfo);
    if (it == vfos.end() || !it->second.lvl.t) return false;
    *out = it->second.lvl;
    return true;
}

bool moment(int vfo, Moment* out) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = vfos.find(vfo);
    if (it == vfos.end() || !it->second.mo.seq) return false;
    *out = it->second.mo;
    return true;
}

void write_json(std::ostream& o, const Level& L) {
    if (!(L.t > 0 && L.sig > 0 && L.noise > 0 && L.total > 0)) return;
    char b[200];
    snprintf(b, sizeof b, ",\"lvl\":{\"dbfs\":%.2f,\"noise\":%.2f,\"total\":%.2f,\"gain\":%.1f,\"age\":%.1f,\"n\":%d}",
             10 * std::log10(L.sig), 10 * std::log10(L.noise), 10 * std::log10(L.total), L.gain,
             (now_ms() - L.t) / 1000.0, L.n);
    o << b;
}

}  // namespace vlvl
