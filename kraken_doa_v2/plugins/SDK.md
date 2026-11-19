# KrakenSDR decoder plugins: SDK

<!-- SPDX-License-Identifier: MIT - Copyright (c) 2026 KrakenRF Inc. -->
This guide and the SDK (`sdk/`) are under the MIT License (`sdk/LICENSE`):
a plugin may have a licence of its own - see *Licensing a plugin*.

Every decoder in the receiver's **Digital Decoder** (sidebar → 🔐 Digital
Decoder → Mode) is a plugin - the protocols that ship with the suite (P25,
DMR, TETRA, D-STAR, NXDN, Yaesu System Fusion, MPT1327, POCSAG / FLEX pagers, APRS, ADS-B) as well as the ones you add.
Plugins are written by hand or by the **🤖 AI assistant** (the web UI's
right-hand column), which has an LLM agent capture and analyse a signal, then
write, build and test a plugin for it - in Claude Code's sandbox; the receiver
runs a plugin the assistant changed sandboxed too (read-only files, no
network: a plugin needs neither). A plugin folder's `CXXFLAGS` file only
passes optimisation / target / `-D` flags (plugins/Makefile
`CXXFLAGS_ALLOWED`): a flag like `-B` would make the compiler run programs.

## Layout

```
plugins/
├── Makefile              builds every plugin (kraken_doa_v2's `make` runs it)
├── SDK.md                this file
├── sdk/
│   ├── kraken_plugin.hpp the API: kp::Decoder, kp::Host, kp::Info, KRAKEN_PLUGIN
│   ├── kraken_dsp.hpp    header-only DSP / bit helpers (filters, FM demod,
│   │                     clock recovery, sync words, CRC, block codes, Viterbi)
│   ├── plugin_host.cpp   main() of every plugin (live + offline test modes)
│   └── tools/uca_scene.py  test transmissions at known bearings on the array (ID separation)
│       tools/wide_scene.py  narrowband transmitters anywhere in the 2.4 MHz, at known bearings
│                            (a trunked system's control + traffic channels; wide playback entries)
│       tools/make_testsignal.py  builds a <id>/tools/testsignals.json entry for the debug playback
├── lib/                  shared digital-radio library (libkrakendig.a, linked into
│                         every plugin; only what a plugin uses ends up in it):
│                         dig_fec (Golay/Hamming/BCH/RS/Viterbi/CRCs), dig_fsk4
│                         (P25/DMR 4FSK sync), dig_vocoder (IMBE, mbelib AMBE,
│                         TETRA ACELP), dig_common (front ends, the Bridge below),
│                         radar_pri / radar_scan (PRI patterns, antenna scan / main
│                         beam - radar, ssr1030), acars_msg.hpp (ACARS block fields -
│                         acars, vdl2)
├── p25/ dmr/ tetra/ dstar/ nxdn/ ysf/ mpt1327/   the shipped protocol decoders
├── pagers/ aprs/         more shipped decoders (POCSAG + FLEX; APRS was written by the AI lab)
├── adsb/                 ADS-B / Mode S at 2.4 MHz - an example of map points; multichannel: every
│                         antenna combined, per-message MRC / MVDR beams, its own pulse covariances
├── radiosonde/           weather balloon sondes (RS41, DFM, M10/M20, iMet-4/-54, LMS6, MRZ):
│                         several protocols side by side, Reed-Solomon, Viterbi, balloon map points
├── ism/                  ISM band devices (433 MHz etc.) in the whole 2.4 MHz: a burst detector,
│                         each burst decimated to 240 kHz, OOK / FSK demodulator, slicers and 69
│                         small protocol decoders; a talker packet per device; .lock_vfo
├── radar/                pulse radars in the whole 2.4 MHz: a cheap pulse detector, PRI / scan
│                         analysis, ID separation by signature (main-beam pulses as packets)
├── ssr1030/              1030 MHz interrogations (SSR / Mode S / TCAS): Mode S DPSK at 0.6
│                         samples per chip by a sequence estimator against the receiver's
│                         measured response; interrogators told apart by train / level / code
├── acars/                VHF ACARS (ARINC 618): 2400 bit/s MSK on AM, NRZI detected coherently against a
│                         2400 Hz reference, odd parity + CRC-16 BCS with soft-decision repair; a packet
│                         talker per aircraft (registration), ground stations from their squitters
├── vdl2/                 VDL Mode 2 (136.x MHz): D8PSK bursts, unique word correlator, coherent + differential
│                         demodulators side by side, descrambler, header FEC, RS(255,249) errors + erasures
│                         (rs.cpp), HDLC / AVLC, ACARS over AVLC, ground station XID (map); a packet talker
│                         per burst = its sender's address (aircraft ICAO hex, ground station)
├── uat/                  978 MHz UAT (US ADS-B + ground uplink): CPFSK at 2.3 samples per bit,
│                         Reed-Solomon GF(256), FIS-B headers / DLAC text; TIS-B = the ground station's
├── flarm/                FLARM (ID + bearing only: encrypted payload) / OGN trackers (LDPC soft
│                         decoding, TEA de-whitening, stealth honoured) on every channel in the 2.4 MHz
├── analog_id/            Radio IDs on analog FM (was ctcss/): CTCSS tones / DCS codes (FFT search +
│                         narrowband tracker, Golay DCS words) and the in-band ID bursts - MDC1200
│                         (coherent MSK), FleetSync 1200 / 2400 / II, 5-tone selcall, DTMF, tone
│                         bursts; a transmission talker named by the radio's ID, else its tone;
│                         ID / tone squelch, no voice
├── beacon406/            406 MHz distress beacons (Cospas-Sarsat): bursts on every channel of a 100 kHz
│                         VFO, biphase-L demodulator, BCH syndrome tables, every protocol; a packet
│                         talker per 15-hex ID on its own channel; distress vs self-test
├── lora/                 LoRa CSS in the full 2.4 MHz (lock_vfo): per-channel dechirp receiver
│                         (SF7-12, BW 125/250/500, Gray/interleave/Hamming/whitening/CRC), Meshtastic
│                         (default-key AES-CTR decrypt + protobuf) and LoRaWAN (clear headers); a packet
│                         talker per node / DevAddr, the relaying node when a mesh frame was repeated
├── wildlife/             VHF wildlife collars / tags (pulsed CW) at the VFO's OWN rate (sample_rate 0:
│                         a 2.4 MHz band survey or a narrow VFO): pulse detector + edges / carrier from
│                         the raw samples, pulse trains (period, modes from the collar's own rate),
│                         region / vendor profiles (profiles.hpp), coded (Lotek / Motus-style) tags;
│                         a packet talker per collar on its 1 kHz channel
├── rds/                  FM broadcast at the VFO's own rate: every antenna combined before the FM
│                         discriminator (multichannel), RDS / RBDS (biphase BPSK, block sync + soft
│                         correction, every group type), RDS2 streams, the MPX's subcarriers classified
│                         (SCA FM audio / FSK data / DARC), SCA or main-programme audio; a packet talker
│                         per station (PI) per RDS group
├── dab/                  DAB / DAB+ ensembles in the 2.4 MHz (lock_vfo): mode I OFDM at the native rate
│                         (2400-point FFT, no resampler), FIC / MSC Viterbi, Layer II (vendored minimp3)
│                         and DAB+ (the user's libfdk-aac, loaded at run time) audio; a packet talker
│                         per transmitter of the single frequency network by its TII code, with a
│                         spatial covariance of that code's own carriers (multichannel)
├── analogtv/             off-air analog TV (PAL 625/50, NTSC 525/60, SECAM=PAL geometry): negative-AM
│                         envelope, broadcast H/V sync recovery, interlaced raster -> a grayscale luma
│                         PICTURE on the decoder tab's video canvas (host.video_frame; lock_vfo, the
│                         2.4 MHz slice gives a soft ~150 px/line image). Shares plugins/lib/video_raster
├── tempest/              screen-leakage (TEMPEST / Van Eck) reconstruction: full-band AM-magnitude,
│                         autocorrelation auto-detection of frame rate + line count snapped to VESA/CVT
│                         modes (pixel_clock = htotal x vtotal x fps), manual overrides, heavy frame
│                         averaging -> a PICTURE (host.video_frame); multichannel -> the source's bearing
│                         as a "screen" talker packet. lock_vfo + manual_only. Shares video_raster
└── <id>/                 ONE plugin = one folder, named by its id
    ├── decoder.cpp       required: the decoder class + KRAKEN_PLUGIN(...)
    ├── *.cpp / *.hpp     optional extra sources (all .cpp files are compiled)
    ├── CXXFLAGS          optional: extra compiler flags for this plugin (e.g.
    │                     -funroll-loops; the defaults are already -O3
    │                     -fno-math-errno -fcx-fortran-rules, -mcpu=native on a Pi)
    ├── tools/            test-signal generator (real framing / FEC / CRC, several
    │                     transmitters with their IDs) - see ID separation below
    ├── README.md         what it decodes, ID separation, sources/specs used, test results
    └── build/decoder     the executable (generated, not part of the plugin)
```

The id is the folder name: lowercase `a-z 0-9 _ -`, at most 32 characters,
not `sdk` or `lib`. It must match `.id` in `KRAKEN_PLUGIN`.

## How it runs

`make` links each plugin's sources with `sdk/plugin_host.cpp` into
`plugins/<id>/build/decoder`. When a VFO's Digital Decoder is set to the
plugin, kraken_doa starts that executable (`decoder --serve`) and streams the
VFO's complex baseband to it over a pipe; the plugin answers with facts,
events and audio. Consequences:

- A crash, hang or runaway allocation in a plugin cannot take the receiver
  down. kraken_doa reports it ("crashed (Segmentation fault) - last output:
  ...") and restarts the plugin with a back-off. The plugin's stderr is
  shown in the panel ("Plugin output").
- **Never write to stdout**: in live mode it carries the binary protocol. The
  host redirects `printf` / `std::cout` to stderr, but use `host.log()`.
- Rebuilding (`make`) while it runs is safe: the binary is replaced
  atomically, and running decoders restart on the new one within ~2 s.
- Every VFO with the plugin selected runs its own process (one decoder
  object per process); a VFO in Auto detect runs one per ticked plugin.
- If the plugin falls more than ~1 s behind, samples are dropped (counted).

## The API

```cpp
#include "kraken_plugin.hpp"
#include "kraken_dsp.hpp"     // optional helpers

class MyDecoder : public kp::Decoder {
public:
    explicit MyDecoder(kp::Host& h) : Decoder(h) {}
    void process(const kp::cf* x, size_t n) override;   // complex baseband
    void process_multi(const kp::cf* const* x, int m, size_t n) override;   // optional: every antenna (below)
    void reset() override;                               // optional
    void option(const std::string& key, const std::string& value) override;  // optional
};

KRAKEN_PLUGIN(MyDecoder, {.id = "mydec",
                          .name = "My decoder",           // shown in the mode list
                          .category = "Two-way radio",    // its group in the mode list (below)
                          .description = "one line",      // the panel's help text
                          .version = "1.0",
                          .sample_rate = 48000,           // rate process() receives (0 = the VFO's own)
                          .min_vfo_rate = 12500,          // VFO bandwidth needed
                          .author = "...",
                          .options = {{"slot", "Timeslot", "0", "0=Both|1=Slot 1|2=Slot 2",
                                       "help text"}},     // settings in the panel
                          .map = false,                   // true: sends map points (below)
                          .manual_only = false,           // true: never runs in Auto detect
                          .fixed_freq_hz = 0,             // > 0: the frequency the VFO is tuned to
                          .voice = false,                 // true: decodes voice - the panel offers Listen
                          .messages = false,              // true: sends text messages (host.message)
                          .talkers = false,               // true: reports who transmits (host.talker)
                          .lock_vfo = false,              // true: the VFO stays on its tuner's centre (below)
                          .multichannel = false,          // true: takes every antenna (process_multi, below)
                          .video = false,                 // true: rebuilds a picture (host.video_frame, below)
                          .squelch_hold_s = 0,            // > 0: the Digital squelch's hold (below)
                          .trunking = false})             // true: reports a trunked system (host.grant, below)
```

The fields of `kp::Info` must be given **in this order** (C++20 designated
initializers); any of them can be left out.

**Category**: the heading the decoder is listed under in the mode lists
(VFO card, decoder tab, saved-frequency editor). The shipped plugins use
`Two-way radio`, `Aviation`, `Marine & distress`, `Paging & packet`,
`ISM & IoT`, `Telemetry & tracking` and `Broadcast & cellular` (listed in
that order); pick one of them where it fits - case doesn't matter - or a
name of your own (listed after those, by name). Without one the plugin is
listed under `Other`, last.

**Options** (`kp::Option`: key, label, default, choices, help, hidden) are the
decoder's own settings. The panel shows them while the plugin is selected (in
Auto detect: once it has locked onto this plugin) - `choices` = `"value=Label|..."`
makes a list, `""` a text field; exactly two choices with the values `0`
and `1` (`"0=Off|1=On"`) make an on / off button ("● Label: On"; a
choice's text after its name in brackets is left off the button and shown
in its tooltip). A `choices` of the form `"range:MIN:MAX[:STEP]"` (e.g.
`"range:10:95:5"`, STEP defaults to 1) makes a **slider** (`<input
type="range">`) with the live numeric value shown beside it; the default and
saved value come from `default` / the stored value, and dragging it sends the
number through `option(key, value)` like any other option (debounced while
dragging). Use it for a numeric knob such as the video `quality`. They arrive
through `option(key, value)`: the default when the plugin starts, then
whatever the user picks (`""` = back to the default). They are saved per VFO.

`hidden = true` (the last field: `{"service", "Programme", "", "", "help",
true}`) draws no control for the option. It is set another way: a click on
a table row whose key is one of its run-time choices (below, *Tables*), a
saved setting, a debug-playback catalogue's `opts`, the AI assistant's
`set_decoder_option` (`DIGITAL_OPT` accepts any declared-or-not
`<id>.<key>`). dab's Programme: the services table is the list, so a second
control was redundant. Say in the plugin's `description` (shown under the
settings row) how it is set. `decoder --info` marks it (`"hidden":true`, a
sixth `options_tsv` field `1`); a kraken_doa from before reads five fields
and shows the control as usual - fine, the option still works there.

A list that is only known at run time - the programmes of the DAB ensemble
being received - comes from `host.option_choices(key, {{value, label},
...})` instead: the panel then draws that option as a list of these
choices, whatever its declaration says (unless it is `hidden`; dab's
`service` is declared as a hidden text field, so an older kraken_doa still
offers a text field / the list, the current one neither). Pairs rather than the `"value=Label|..."` text, so a
label from the air with `|` or `=` in it can't add entries. A new list
replaces the previous one; `{}` takes it back to the declaration. Limits:
100 choices, values <= 100 bytes without control characters (others are
dropped - they couldn't be set), labels <= 80 bytes (cut at a character
boundary; `""` = the value), 32 KB for all of a decoder's lists. Send it
when the list changes, not on every update. It is run-time state: the host
forgets it when the plugin restarts, on `reset()` (a retune) and when the
decoder is switched - send it again then; nothing of it is saved. The
setting itself stays whatever text the user or a saved setting gave: if it
isn't one of the values (an older setting, a part of a name), the panel adds
it as an entry of its own so the list shows what is set (you may give it an
entry yourself that says what it selects; a hidden option needs none -
say it in a fact instead, as dab's Programme fact does). Use a choice's
value as the key of its table row and the row becomes a way to pick it
(*Tables* below) - for a `hidden` option the only one in the panel.
Choose values that stay valid (dab: the service's SId, not its position in
the list). An older host ignores the message (wire `OPTION_CHOICES` 42); the
offline test prints the final lists in its summary.

**Auto detect**: when a VFO's decoder is on "Auto detect", every plugin the
user left ticked (sidebar plugin list, "Auto detect"; all are by default)
runs side by side - except plugins that set `.manual_only = true` (they get
no Auto detect tick at all; ADS-B) and plugins whose `min_vfo_rate` is more
than twice the VFO's rate (they couldn't decode there); the one whose `valid()` frames dominate is shown, and only
its voice / carrier offset is used. So a plugin must never call `valid()` on
noise or on other protocols - it would steal the detection. (`Info::auto_detect`
is deprecated and ignored; it is only kept so older plugins compile.)

**Fixed frequency.** A decoder for one frequency (ADS-B: 1090 MHz) sets
`.fixed_freq_hz`: picking it for a VFO sets the narrowest VFO bandwidth of at
least `min_vfo_rate`, puts the VFO on its tuner's centre and tunes that tuner
(in coherent mode the whole array) to the frequency. While the decoder is
selected the VFO is drawn as a single line and can't be dragged; its
frequency, bandwidth and tuner fields are locked.

**Locked VFO, any frequency.** A decoder of the whole band the tuner
receives (ism: every ISM device in the 2.4 MHz; radar: every pulse radar in
it; the cells / Digital TV array plugins: all of a 2.4 MHz VFO) sets
`.lock_vfo = true`
instead: picking it does the same - narrowest bandwidth >= `min_vfo_rate`,
VFO on its tuner's centre, a single line, fields locked - but leaves the
tuner where it is; the user tunes the receiver to the band (a 📍 map
marker's frequency button retunes the tuner then, rather than refusing).

**Every antenna.** A plugin that sets `.multichannel = true` gets every
antenna of the array through `process_multi(x, m, n)` (`x[k][i]` = antenna
k's sample i, m = 2..8, at `sample_rate`) whenever kraken_doa has them: in
coherent mode with DoA on, for a plugin at the VFO's own rate (no
resampling) and no AFC correction. The antennas are sample-synchronous and
on one frequency; their phases and gains need not be calibrated (combine
them blindly - ism uses the main eigenvector of each burst's own spatial
covariance). Otherwise - independent / wideband mode (each VFO its own
tuner), DoA off, an offline test of one channel - it gets `process()` as
usual, so a plugin must work with either and may switch between them at any
time. The default `process_multi` decodes antenna 0. Wire: `SAMPLES_MC`
(uint32 m, then n x m complex int16 sample by sample, value / 8192).
Offline: `decoder --file REC --channels M` (an interleaved recording of
every antenna; `--channel K`: only antenna K through `process()`). (Array
plugins, below, get every antenna in bursts instead - for measurements per
antenna, not decoding.)

**Retuning.** A decoder that searches for its signal beyond the VFO (the
cells plugin looks for an LTE / NR carrier up to 20 MHz away) sets `.tunes =
true` and calls `host.tune(rf_hz)`: kraken_doa then retunes so that rf_hz
sits at 0 Hz of the input - the whole receiver in coherent mode (every VFO
moves along), the VFO's own tuner in independent mode - through the same
FREQ / TUNER_FREQ path a user's tuning takes (saved, sent to heimdall, shown
in every page). Fire and forget: when the retune is done the plugin gets
`reset()` and a new `rf_hz()`; a refused one (out of the tuner range,
wideband scan mode, sooner than 2 s after the last) changes nothing and the
decoder's event log says why. `false` = this host can't (the offline test
prints the request). Each retune makes heimdall recalibrate for a few
seconds - step through a band, never retune continuously, and treat an RF
you didn't ask for as the user's choice. Plugins that don't declare
`.tunes` can't retune.

**Input.** `process()` gets the VFO's complex baseband, centred on the VFO
frequency, already resampled to `Info::sample_rate`, in blocks of a few ms
(any size - keep your own state between calls). Amplitude is arbitrary
(normalize yourself). Choose a sample rate that is an integer multiple of the
symbol rate when that helps (e.g. 38400 = 32 x 1200 = 16 x 2400), and keep it
as low as the signal allows: it is the main CPU cost. `min_vfo_rate` is the
VFO bandwidth the signal needs; the panel warns when the VFO is narrower.

**The VFO's own rate.** `.sample_rate = 0`: no resampling - `process()`
(and `process_multi()`) get the VFO's samples at whatever bandwidth the user
set, and `host.sample_rate()` says which, from the constructor on (kraken_doa
starts the plugin as `decoder --serve --rate HZ`; a change of the VFO's
bandwidth restarts it). For a decoder of signals anywhere in the VFO that
works at any width - wildlife: a 2.4 MHz band survey or a 10 kHz VFO on one
collar - instead of a 2.4 MHz input that a narrow VFO would be upsampled to.
The offline test feeds the file at its own rate (no `--offset` resampling).
Older hosts refuse such a plugin ("bad sample_rate").

**Output, through `host`** (`kp::Host`):

| call | effect |
|---|---|
| `host.fact(key, value)` | a row of the panel's table (network IDs, last message, counters...). Same key = update; `""` removes it. Keys <= 64 chars, values <= 400 |
| `host.event(text, dedup_s = 2)` | a line in the event log; identical texts within `dedup_s` s are dropped (use a large value for repeating broadcasts) |
| `host.valid()` | one frame/message **passed its checks** (sync + CRC/FEC). Drives "Receiving ..." and the frame counter. Never call it for unverified data |
| `host.valid(start_s, end_s)` | the same, with the frame's samples (`host.time()` of its first sample and of its end) - **prefer it**: the VFO's **Digital squelch** opens only on these frames and takes the DoA from exactly these samples (see below) |
| `host.audio(pcm, n)` | decoded audio, 8 kHz mono float (+-1). Only played when the user listens (Demod "Digital") or the VFO's "🗣 Transcribe" is on (Voice to Text writes it down); skip the work when `!host.voice_wanted()` |
| `host.audio_channel(ch)` | optional: the talker channel (`kp::Talker::channel`, e.g. the DMR timeslot) the following `audio()` is from, when several transmissions run at once and you play one - Voice to Text then names the radio on that channel. Call it when it changes; -1 = unknown |
| `host.voice_state(s)` | short call status shown next to the Listen button |
| `host.freq_error(hz)` | measured carrier offset (+ = above the VFO centre): the host slowly re-centres the input. Optional |
| `host.verbose()` | also report the messages a control channel repeats constantly: the decoder tab's "Show all messages" tick, or its Logging at "Log All" (while logging to disk is allowed) |
| `host.time()` | seconds of input processed so far (sample clock) - use it for timeouts, not the wall clock |
| `host.sample_rate()` | the rate `process()` gets (Hz): `Info::sample_rate`, or the VFO's for a plugin that declares 0 |
| `host.log(text)` | debug output (stderr) |
| `host.map_point(p)` | a position on the web UI's 🗺 Map (`kp::MapPoint`, below) |
| `host.map_remove(id)` | takes a point off the map |
| `host.station(&lat, &lon)` | the receiver's location (sidebar → Station Information), false if it has none |
| `host.rf_hz()` | the radio frequency (Hz) at 0 Hz of your input - the VFO's centre plus the host's frequency correction - NAN when unknown (offline test without `--rf`); +f in your input is on `rf_hz() + f` (`host.rf_inverted()`, the removed "invert" option, is always false - kept so older plugin sources build). Name the channel you receive (`dme`: a DME channel from its reply frequency), or find the channels inside a wide VFO. A retune also calls `reset()` |
| `host.table_columns(cols)` | headings of a live table in the panel (e.g. the aircraft ADS-B hears) |
| `host.table_row(key, cells)` | adds / updates (same key) a table row; rows not updated for 10 min (or `table_keep`) drop out |
| `host.table_remove(key)` | removes a table row |
| `host.table_keep(seconds)` | how long a row stays without an update (60 s .. 7 days, default 10 min) - things that report rarely want long (ism: its "Keep devices" option, 24 h) |
| `host.table_detail(key, lines)` | a row's details: label / value pairs the panel shows when the row is opened (▸) |
| `host.message(from, text)` | a free-text message the decoder received (a pager text): kraken_doa looks for a street address in it and shows it as an incident (set `.messages = true`) |
| `host.raw_wanted()` / `host.raw(text)` | one raw frame as text for the decoder data log (only while `raw_wanted()` - the VFO's Logging is "Log All"; written to `decoder_logs/<plugin id>/<plugin id>-YYYY-MM-DD.jsonl`) |
| `host.talker(t)` / `host.talker_end(at_s, channel)` | who is transmitting (`kp::Talker`: radio ID, label, start / confirmed end in `host.time()`, channel, packet): the DoA is split per radio / aircraft (set `.talkers = true`, below) |
| `host.talker_forget(id)` | drops a talker now - its bearing, history and row in the 📡 DF list (wire TALKER_FORGET 33; ism: its unknown devices when "Unknown signals" is set to Ignore) |
| `host.option_choices(key, choices)` | the choices of one of your options known only at run time (`{{value, label}, ...}`; dab: the ensemble's programmes): the panel shows the option as that list; `{}` = none (see Options above; wire OPTION_CHOICES 42, ignored by older hosts) |
| `host.video_frame(f)` | a reconstructed picture (`kp::VideoFrame`: width, height, format, pixels, label) drawn on a canvas in the decoder's tab - analog TV, screen-leakage (TEMPEST); set `.video = true` (see below) |
| `host.plot(p)` | a line plot (`kp::Plot`) in the decoder's tab, or several, beside the video display, and one a click on sets an option (see *Array plugins: ... plots* below) |
| `host.trunk_system(s)` | the trunked system this VFO hears (`kp::TrunkSystem`: network key, name, site, control channel or not) - see *Trunked systems* below (wire TRUNK_SYSTEM 43) |
| `host.trunk_channel(c)` / `host.control_channel(c)` | a channel the system announced (`kp::TrunkChannel`: name, number, downlink / uplink frequency, slot) / the system's control channel is now there (wire 44 / 45) |
| `host.grant(g)` / `host.grant_end(c)` | a channel grant heard (`kp::Grant`: channel, talkgroup / unit, source, flags, the traffic decoder and its options) / the system took the channel back (wire 46 / 47): kraken_doa can follow the call |
| `host.call(c)` | on a traffic channel: the call it carries now (`kp::Call`: ACTIVE / ENDED / RELEASED, target, source, slot, flags; wire 48) |

**Voice.** Set `.voice = true` if the decoder sends audio (`host.audio`):
only then does its panel tab offer the Listen button.

**Text messages → incidents.** A decoder of text messages (pagers...) can
pass each one to `host.message(from, text)` and set `.messages = true`.
kraken_doa finds a street address in the text and looks it up online
(OpenStreetMap Nominatim) around the station; the result is an incident in
the decoder's tab and, with "Plot on map", on the 🗺 Map. The plugin needs
no map code (and must not use the network itself).

**Decoder data log.** The user can log what a decoder reports to disk (the
decoder tab's Logging: Log / Log All, while ⚙ Setup Settings → Decoder
Logging allows it; one JSON Lines file per day): your events, text messages
and map points are logged without any code in the plugin. Under "Log All"
`host.verbose()` is on too, so the messages you only report with verbose
are written then. Raw frames are the one thing to add: while `host.raw_wanted()` is
true, pass each frame that passed its checks to `host.raw(text)` - one line,
printable, compact (hex for binary frames, e.g. ADS-B `"8D4840D6... -14.8"`
= the Mode S message + level in dB; APRS the TNC2 packet; pagers "POCSAG
RIC n F<f> <baud>" + the message codewords, "FLEX <speed> <cycle.frame>
<phase> capcode n V<type>" + the vector and message words). Check `raw_wanted()` first: building
the text costs CPU and it is usually off. The offline test prints raw frames
with `--raw`. Map points are logged with their `info` lines (as a JSON
object), so put every detail worth keeping there ("Squawk: 7700"). Start
the event text of anything unusual or alarming with "⚠" (ADS-B:
emergencies, squawk changes, extreme climbs) - the log, and anyone reading
it, can pick those out.

**Map points.** A decoder that learns positions (aircraft, APRS stations,
vehicles...) puts them on the 🗺 Map in the right-hand pane:

```cpp
kp::MapPoint p;
p.id = "4CA2B1";            // stable key: the same id again moves the marker
p.lat = 53.42; p.lon = -6.27;
p.label = "RYR12AB";        // text next to the marker ("" = the id)
p.kind = "aircraft";        // aircraft | vehicle | ship | person | station | balloon | point
p.heading = 123;            // degrees true (rotates the marker); NAN = unknown
p.altitude_m = 11278;       // NAN = unknown (shown next to aircraft in ft)
p.speed_kmh = 830;          // NAN = unknown
p.info = "Callsign: RYR12AB\nSquawk: 1234";   // "Key: value" lines for the popup
p.ttl_s = 60;               // removed after this long without an update
host.map_point(p);
```

Send updates as often as you like (the browsers get them once a second);
call `map_remove(id)` when you know a point is gone, the ttl covers the
rest. Set `.map = true` in `KRAKEN_PLUGIN` so the panel offers "🗺 Plot on
map" for the decoder from the start. Nothing reaches the map until the user
ticks "Plot on map" in the decoder's tab (off by default, saved per VFO);
the points are kept meanwhile. kraken_doa clears a decoder's points
when the VFO is retuned or the decoder switched (your `reset()` runs then).
The offline test prints the final map points; `--station LAT,LON` sets the
receiver location there. The shared library's decoders (`lib/`, written
against `dig::Report`) report positions with `Report::map()` and read the
station with `Report::station()` - the DMR (GPS info LC) and D-STAR (GPS /
DPRS slow data) plugins do; APRS calls `host.map_point` directly.

**Talkers → DoA per radio.** A decoder that knows WHO transmits (a P25
unit ID from the link control, a DMR source ID...) reports it, and
kraken_doa cuts the VFO's signal at those boundaries: each radio gets a
bearing computed only from its own samples, its own history and its own
heat map while driving, picked in the 🗺 Map's 📡 DF panel. Set
`.talkers = true` and, for every frame of a transmission that passed its
checks:

```cpp
kp::Talker t;
t.id = "2010621";           // the radio, stable (<= 32 chars)
t.label = "TG 3038";        // shown next to it
t.start_s = call_start;     // host.time() where the transmission began (its first frame)
t.end_s = frame_end;        // host.time() where this frame ENDS - not "now"
host.talker(t);
```

and `host.talker_end(at_s)` when it ends (the terminator frame, or frames
stopped coming - use a timeout on `host.time()`). A different id ends the
previous talker where the new one starts. Give exact sample times: the
frame boundaries in your receiver's own sample count, converted with
`host.time()` at the receiver's sample 0 (`host.time()` during `process()`
is the time of that block's first sample) - never the moment a frame was
decoded, which comes a frame or more later. Only the samples between
`start_s` and the latest `end_s` are used, with a 40 ms guard at both ends,
so an early or late boundary costs a frame, while a wrong one mixes radios.
The p25 plugin is the reference (`p25/p25.cpp`: `voice_frame`,
`set_talker`, `tick`); nxdn, ysf, dstar and dmr do the same. Plugins built on
`plugins/lib` get the time conversion from `dig::Bridge::talkers(rate)` +
`restart_clock()` (call it in `reset()`). The offline test prints the
transmissions it would cut ("talkers:" in the summary).

Several transmissions at once on one frequency (DMR's two timeslots): give
each its own `t.channel` (0..7) and end it with `host.talker_end(at_s,
channel)`; a new id ends only its own channel's talker. Frames holding two
channels' radios go to neither. On such a channel, report a transmission
whose radio isn't known yet with `t.id = "?"` (the dmr plugin does until
the LC names it, p25 on a Phase 2 timeslot too): its samples then stay out
of the other channel's talker, and the real id later takes over from the
same start. A protocol with one transmission at a time needs neither.

Short packets from many talkers at once (ADS-B / Mode S: 64-120 us per
message, dozens of aircraft) don't fit that model - a MUSIC frame holds
many of them. Report each packet that passed its checks with `t.packet =
true`, `start_s` = the packet's FIRST SAMPLE and `end_s` = its end, to the
sample (`host.time()` of the block + the packet's index in it / the rate):
kraken_doa then computes the covariance from exactly those samples and
averages each talker's packets over the last ~2 s (its lobe on the map).
No `talker_end`. A packet may be at most 0.25 s long (kraken_doa drops
longer ones). The adsb plugin is the reference (`talker_packet`); its
start times are within half a sample of the true ones. Which talker to
report and how to test it: *ID separation* below. Two optional packet
fields: `t.freq_hz` / `t.bw_hz` = the packet's channel inside your band
(Hz from what you receive at 0 Hz) - kraken_doa then filters that channel
out of every antenna first, so talkers on neighbouring channels of the
same VFO don't mix (the ais plugin: -25 / +25 kHz, 16 kHz wide; on a VFO
wider than 500 kHz only channels <= 250 kHz wide - the ism plugin's devices -
are filtered, kraken_doa then keeps the stream as 16-bit samples for 2.5 s:
120 MB at 2.4 MHz with 5 antennas; wider ones use the whole band; a channel
<= 10 kHz far narrower than the VFO - a wildlife collar's 1 kHz in 2.4 MHz -
is mixed down and summed to ~16 x bw first, then filtered: the plain 401-tap
filter passed ~36 kHz at 2.4 MHz); `t.avg_s`
= how long to average a talker's packets (default 1.5 s for fast movers
that send often; ais: 20 s); `t.keep_s` = how long kraken_doa keeps the
talker - its bearing, history and row in the 📡 DF list - after its last
packet (default 2 min, or 30 x avg_s; ism: its "Keep devices" option, 24 h;
at most 500 talkers, the least recently heard go first). A talker that says where it is can also give
its position AT THE PACKET'S TIME - `t.lat` / `t.lon` (WGS84) / `t.alt_m`
(metres above sea level), NAN = unknown: kraken_doa's ✈ array calibration
uses such packets as transmitters at known directions (the adsb plugin
moves the last position on with the velocity: `packet_position`). Only
give one that is good to some tens of metres. The offline test counts
them ("talker packets:", "N with position"; each one with `--verbose`).

**A packet's own covariance (separation by code).** A `multichannel`
plugin (every antenna, below) that can isolate ONE transmitter's signal in
the samples itself - spread spectrum: the lora plugin correlates every
antenna with each decoded symbol's own chirp, so other transmitters at the
same moment stay spread out - sends that covariance with the packet:
`t.m` = the antennas, `t.cov` = m x m row-major (any scale). kraken_doa then
takes it as it is (MUSIC with one source on the talker): no channel filter,
no stream kept for the packet, and it is NOT dropped for overlapping another
talker's packet on the same channel (the other one, cut from the stream,
still is). Ids, labels, avg_s, keep_s work as for any packet. Wire: TALKER
fields 14 / 15 (m, "re,im,..."); an older kraken_doa ignores them and cuts
the packet from its stream as before. The offline `--uca` bearings use it
too ("[n by code]"). Only send it when it really holds one transmitter
(the lora plugin's `code_cov`: outlier symbols out, the main eigenvalue
>= 40 % of the trace).

**Valid frames with their samples → the Digital squelch.** A VFO's squelch
can be set to "Digital" while its decoder runs: it opens only while the
decoder reports valid frames (and 1.5 s after the last), and the VFO's DoA
is computed from exactly the samples of those frames - nothing before,
between or after them (noise, another signal, an interferer on the same
channel) gets into the bearing. For that, report each valid frame WITH its
samples: `host.valid(start_s, end_s)` instead of `host.valid()`, where
`start_s` / `end_s` are `host.time()` of the frame's first sample and its
end (the first sample of the block in `process()` is `host.time()`; add
the frame's index in your input / `sample_rate`). A frame's span may also
take in what proved it (the sync word, a preamble); keep it under 3 s.
Talker spans and packets (`host.talker`) count as confirmed samples too.
The squelch stays open 1.5 s after the last valid frame - right for frames
a few hundred ms apart; a decoder of a continuous signal that reports short
pieces (analog_id: every 50 ms while the tone is there) sets
`.squelch_hold_s` (analog_id 0.35), so the squelch closes soon after the
signal ends instead of passing 1.5 s of noise after every transmission.
The shipped plugins: the lib protocols report through
`RxContext::valid_at` (`dig::report_valid(ctx, mode, start, end)` with the
receiver's own sample indexes; `Bridge::clock(rate)` + `restart_clock()`
map them - `Bridge::talkers(rate)` includes it), pagers reports each synced
POCSAG batch and each FLEX block, aprs / v23_telemetry each frame from its length, radiosonde each
frame from its FSK receiver (`FskRx::clock`). `decoder --file` counts
them ("valid frames with their samples: N, covering X s"; each one with
`--verbose`).

**Tables.** A decoder that tracks many things at once (aircraft, stations,
radios) can show them as a table above its facts: `table_columns()` once
(the constructor), then `table_row(key, cells)` whenever an entry changes -
at most every second or so per row; the page sorts by any column and adds a
"Seen" column (time since the row's last update). Use the same key as the
entry's map point: clicking the row then shows it on the map (as the key of
a talker: selects it in the DF panel). A row whose key is one of the values
you sent with `option_choices` selects that choice when clicked - the same
as picking it in the option's list - and, for a `voice` plugin, turns
Listen on (dab: a programme's row, key = its SId); the page marks the row
whose key is the option's current value. The first that applies counts:
map point, talker, choice. kraken_doa
empties the rows when the VFO is retuned or the decoder switched. The
offline test prints the final table. Whatever doesn't fit the columns goes
into the row's details: `table_detail(key, {{label, value}, ...})` after
its `table_row` (a detail for a row that doesn't exist yet is dropped),
again whenever they change - at most 200 lines (labels <= 80, values <= 400
characters); they go with the row. The page shows a ▸ in front of such a row
that opens them under it (the cells plugin: every SIB field of a cell). They
ride with the table in every status (about once a second), so keep them to
what a person reads - not hundreds of rows' worth.

**Array plugins: every antenna, spatial talkers, plots.** A decoder that
measures something PER ANTENNA (the dtv plugin's path separation: each
antenna's channel impulse response; the cells plugin: each LTE / NR cell's channel from its
own reference signals) sets `.array = true`: instead of `process()` it gets
`process_array(const kp::ArrayBlock&)` - `b.m` antennas (the array's
elements in coherent mode, 1 outside it), `b.n` samples each in `b.ch[a]`,
at the VFO's own rate `b.rate` (no resampling, no AFC; `sample_rate` is
ignored), `b.t0` = `host.time()` of the first sample, `b.gap` = a gap
before this block. The samples come in bursts of `.array_burst_s` every
`.array_period_s` (default 0.25 s every 1 s; contiguous inside a burst; a
2.4 MHz VFO with 5 antennas is 48 MB/s on the pipe, so don't ask for all of
it unless you need it) and not while heimdall calibrates or right after a
retune. An array plugin is `manual_only` (it never runs in Auto detect).

What it finds about a transmitter's DIRECTION goes back as its spatial
covariance: `host.spatial(kp::Spatial)` with `id` (stable while you track
the transmitter), `label`, the samples' span (`start_s` / `end_s`), `m`
and `cov` (m x m, row-major, any scale) - plus `delay_us`, `power_db`,
`snr_db`, `ref` if they mean something. kraken_doa runs MUSIC with ONE
source on it (its array settings and calibration) and makes it a talker of
the VFO: its own lobe, bearing history and heat map on the 🗺 Map's 📡 DF
panel, listed with the other talkers. Report each transmitter once per
burst; the host averages (`avg_s`, default 3 s). A talker you stop
reporting stays listed for 2 minutes - when you KNOW it's gone (lost, or
the user turned your separation off) call `host.spatial_remove(id)` (`""`
= all of yours): it leaves the DF panel at once (wire SPATIAL_REMOVE 39,
ignored by older hosts).

`host.plot(kp::Plot)` shows a line plot in the decoder's tab (one per VFO
running it) and, when its markers name talkers, in the 📡 DF panel of the
map: `x0 + i dx` against `y[i]` (<= 1000 points), units, an optional
dashed `threshold`, `markers` {x, label, talker} - clicking a marker with a
talker shows that talker alone on the map. The latest plot replaces the
previous; send at most a few a second (the pages get them about once a
second, with the tables). A table row whose key is a talker id selects
that talker the same way.

**Several plots, beside the video, set an option by clicking.** More
fields of `kp::Plot` (any plugin, not only array ones):

| field | effect |
|---|---|
| `id` | the plot's own name (`[A-Za-z0-9_.-]`, <= 32 chars): each id is a separate plot, replaced only by the next with the same id (at most 8 per plugin); shown in id order. `""` = the plugin's one plot, as before |
| `place` | `kp::PLOT_TAB` (default: the decoder tab's row of plots) or `kp::PLOT_BESIDE_VIDEO`: in a column right of the VFO's video display (`.video` plugins, *Video frames* below), as tall as the picture, wrapping below it in a narrow panel |
| `click_option` | the key of one of your `Info::options`: the page shows the value under the pointer and a click sets that option to it - through the same decoder-option path as typing it into the option's field (saved per VFO, shown in every browser, `option(key, value)` in the plugin). The value is sent as a plain number |
| `click_also` | `"key=value"`: another option the click also sets, when it isn't so already - the one that makes the value take effect (the tempest plugin: `mode=manual`). For a list option the value must be one of its choices. Sent after the clicked value |
| `click_snap` | > 0: the value snaps to the curve's highest point within +- this many x units of the pointer (the page widens it to at least 4 screen pixels); Shift+click takes the exact position |
| `click_step` | > 0: the value is rounded to a multiple of this (`1` = whole numbers) and printed with that many decimals |

The markers keep their meaning there; the first one's label is shown in
the plot's caption as the value in use. Wire: such a plot goes as
`PLOT_EX` (41, the extension fields first, then a PLOT payload); a
kraken_doa older than it ignores the message (the plot just doesn't show),
and a plot without the new fields is still sent as `PLOT`, so it shows on
older and newer receivers alike. A page older than them never shows a
named plot. Plots beside the video are only seen while the video display
is: skip their work while `host.video_wanted()` is false. The tempest
plugin is the reference (`send_fps_plot` / `send_lines_plot`: its frame
rate and total-lines graphs, which set its `fps` / `lines` options).

### Video frames

A decoder that rebuilds a picture from the signal - analog TV (PAL/NTSC
luma), screen-leakage reconstruction (TEMPEST) - sets `.video = true` and
sends each frame with `host.video_frame(kp::VideoFrame)`: `width` x `height`
pixels, `format` 0 = GRAY8 (1 byte/pixel, 0 black .. 255 white) or 1 = RGB24
(3 bytes R, G, B), `pixels` row-major top-left first, and an optional `label`
drawn as a caption (e.g. "1024x768 @ 60 Hz"). You always pass **raw pixels**;
the host compresses them for you. The panel shows a canvas per VFO running the
plugin; the latest frame replaces the previous one, so send at most a few a
second.

The host compresses each frame to **JPEG** (libjpeg-turbo) before it goes on
the wire, and the browser decodes it natively (built in, no download). A GRAY8
frame is encoded as a **native grayscale JPEG** (one plane - the fastest and
smallest path), an RGB24 frame as a 3-component JPEG. This makes large frames
practical: a compressed screen is a few KB, so pictures up to 4K (3840x2160)
pass where the raw raster would be far over the 1 MB pipe limit
(`kp::wire::MAX_PAYLOAD`). Two optional hints on `kp::VideoFrame` steer it:
`quality` (1..100, -1 = the host default ~60) for the lossy encode, and
`lossless = true`, which uses maximum JPEG quality for a frozen or heavily
averaged still (JPEG has no truly lossless mode). A plugin that lets the user
set the quality should expose it as a **slider** (see the `range:` option form
below), feeding the value straight to `VideoFrame.quality`. The host also keeps
a hash of the last frame it sent and **skips an identical one**, so a static
screen costs ~no bandwidth - just keep sending frames. The skip is
time-bounded: an unchanged frame is still re-sent about **every 1.5 s** as a
keyframe (a few KB/s for a static screen), so a frame dropped in transit
(back-pressure) self-heals within that interval and a newly-opened video panel
fills in without waiting for the picture to change. A changed frame, or any
geometry change, always sends immediately.

Frames are only wanted while a browser shows the video display: kraken_doa
tells the plugin (`host.video_wanted()`, the OPTION `video_wanted=0|1` on the
wire), and while it is false `video_frame()` returns at once - no hash, no
encode, no pipe traffic. A decoder can check `host.video_wanted()` to skip the
render / scaling that only feeds the picture, but should keep reconstructing
(averaging) so the picture is ready the moment someone opens the panel; when
it turns true the host forgets its last-frame hash, so the next frame goes out
immediately even if unchanged. It is true on a host that doesn't say (older
kraken_doa builds, the offline `--file` test).

If the host was built without libjpeg (the `make` note says so), it falls back
to sending raw pixels unchanged; then the whole frame must stay under 1 MB (a
GRAY8 raster up to ~1000x1000, RGB24 ~590x590) - downscale a bigger picture, as
a frame that still won't fit is dropped (logged once), never truncated. Either
way the hints are harmless to set. Older hosts without the video channel drop
the frame.

The offline test runs array plugins on multichannel recordings (cf32,
sample-major: antenna 0..m-1 of sample 0, then of sample 1, ...; `channels`
in the `.json` sidecar or `--channels M`), in bursts like the live host
(`--all` = every sample back to back); `--uca R` (metres) puts the
antennas on kraken_doa's clockwise UCA and prints each spatial talker's
MUSIC bearing at the end. `plugins/dtv/tools/sfn_gen.cpp` writes synthetic
single frequency networks in that format, `plugins/cells/tools/cell_gen.cpp`
LTE / NR cells, `plugins/dab/tools/dab_gen.cpp` DAB networks (each
transmitter with its own TII code - packet talkers with their own
covariances, `t.cov`).

## Trunked systems (grants, calls: trunk following)

A trunked radio system (P25, DMR Tier III / Capacity Plus, NXDN, TETRA,
MPT1327) hands out its traffic channels call by call on a control channel.
A plugin that reads such a control channel reports what it hears there in
a structured form - besides its usual events and facts, which stay as they
are - and kraken_doa's **trunk following** puts a temporary voice VFO with
the right decoder on each granted channel (inside the receiver's 2.4 MHz)
for the length of the call, so every call gets heard, transcribed and
direction-found. How the host does it (the VFOs, the user's channel table,
its commands) is in `kraken_doa_v2/docs/dev/trunk-following.md`; this is
the plugin's side. Set `.trunking = true` (the plugin list shows it), then:

| call | when |
|---|---|
| `host.trunk_system(s)` | the system's identity is known or changed - and again every few seconds (the host keeps the latest) |
| `host.trunk_channel(c)` | the system announced a channel (a band-plan / channel-list entry, a neighbour's channel): the host keeps it in the system's channel table |
| `host.control_channel(c)` | the control channel is (now) somewhere else - a rest channel that moved, an announced change: the host moves this VFO there when it can |
| `host.grant(g)` | once per grant message HEARD - first grants, grant updates, late-entry messages alike |
| `host.grant_end(c)` | the control channel says the channel is free again (a release / deassignment) |
| `host.call(c)` | on a TRAFFIC channel: a call starts, its target / source / encryption become known or change, it ends |

**`kp::TrunkChannel`** (in all of them): `name` = the channel as the system
numbers it, the SAME text every time for the same physical channel ("1-0412"
for P25 identifier 1 / channel 412, "LCN 5", "412") - the host's key for it,
also to look its frequency up in the user's table: ALWAYS fill it (a message
without one is dropped); `number` = the number alone when the system has
one (-1 none: a "base + n x step" plan the user enters works on it);
`freq_hz` = the DOWNLINK, what a receiver near the site hears - give it
whenever the signalling or a standard band plan determines it (P25
identifier updates, TETRA carrier numbers with the band / offset / duplex
from the system information, DMR absolute parameters), else NAN: never a
guess, the user's channel table then decides; `uplink_hz` = what the radios
transmit on (the repeater's input), only when the signalling or a standard
plan gives it, NAN = not known or simplex - `freq_hz` stays the downlink
(a DF receiver gets the site's bearing on the downlink, the radio's own on
the uplink); `slot` = the TDMA timeslot on it, 0-based (DMR timeslot 1 =
0, TETRA timeslot 1 = 0), -1 = the whole channel.

**`kp::TrunkSystem`**: `key` = the NETWORK's identity, the same on every
site and after a restart (P25 WACN.system "BEE00.14C", TETRA MCC-MNC
"228-8889", an MPT1327 system code), `[A-Za-z0-9._-]` <= 48 (other
characters become `_`), `""` until known - kraken_doa files what the user
knows about the system (channel frequencies, talkgroup names) under your
plugin id + this key, so nothing site- or day-specific goes in; `name`
how to show it, `site` this site ("RFSS 5 site 9", `""` none), `control` =
this VFO is on a control channel.

**`kp::Grant`**: `channel`; `target` = the talkgroup or the called unit as
the system writes it ("1201", "32-1201"), `group` false for a unit-to-unit
(private) call; `source` = the calling unit (`""` if the message lacks it);
`encrypted`, `emergency`, `data` (a data channel - the host doesn't follow
it), `update` = the repeat of a call in progress rather than its start;
`decoder` = the plugin for the traffic channel (`""` = this one); `demod`
= `""` (the decoder's own voice, Demod Digital) or `"nfm"` (analogue FM
speech: MPT1327's traffic channels); `bw_hz` = the traffic channel's width
(0 = the decoder's `min_vfo_rate`); `options` = what the traffic channel's
decoder needs before it has heard it itself, by THAT decoder's
`Info::options` keys - the timeslot, the colour code / NAC / RAN, the
scrambling identity (TETRA's MCC / MNC / colour code, P25 Phase 2's WACN /
system / NAC) - at most 16, keys `[a-z0-9_]{1,32}`, values <= 100 bytes
without control characters. Repeats are expected: send `grant()` for every
grant message you decode, also when the system repeats it every second for
the length of the call - the host de-duplicates them and takes them as the
call's keep-alive.

**`kp::Call`** (traffic channels): `state` = `Call::ACTIVE` while a
transmission is on, `Call::ENDED` when it is over (its terminator, or frames
stopped - the channel may stay assigned for the hang time),
`Call::RELEASED` when the system cleared the channel (clear-down, call
release); `target` / `group` / `source` as in the grant (`""` = not known
yet), `slot`, `encrypted`, `emergency`. Call it when a call starts, when
its target / source / encryption become known or change, and when it ends;
repeating it (once per superframe) is fine. `talker()` and `valid()` stay as
they are - the DoA per radio and the Digital squelch come from them, not
from `call()`.

What a TRAFFIC-channel instance may rely on: kraken_doa starts it as any
VFO's decoder (`decoder = ""`: your own plugin) on the granted frequency and
width; the grant's `options` arrive through `Decoder::option()` before, or
with, its first samples; when the host moves that VFO to another channel
you get `reset()` (as on every retune) and the new options. It doesn't know
it is on a traffic channel unless an option says so - report what you hear
(`call()`, `talker()`, and `grant()` for grants a traffic channel carries).

Limits, both ends (the plugin side cuts or drops, kraken_doa checks
everything again - a plugin is untrusted input): texts lose control
characters and are cut at a character boundary (names 32, target / source
32, system name / site 64, key 48 bytes), a frequency outside (0, 6 GHz] is
NAN, a slot outside -1..15 is -1, a `decoder` that is no plugin id or a
`demod` that is no `[a-z0-9_]{1,16}` is `""`, bad options are dropped.
Delivery: in Auto detect the host takes these only from the plugin it
locked onto (as its voice) - reports before that are lost, so repeat the
system now and then; with the plugin selected, all of them. When the VFO's
decoder is reset (a retune > 100 Hz), stopped, switched, or its process
dies or is rebuilt, the host drops what it had from it - send the system
again once you know it. Older kraken_doa builds ignore all six (wire
TRUNK_SYSTEM 43, TRUNK_CHANNEL 44, CONTROL_CHANNEL 45, GRANT 46, GRANT_END
47, CALL 48); plugins from before send none.

**Offline test.** `decoder --file` prints each call on one line, as the
host receives it (cleaned to the limits), key=value tokens with the texts
quoted - `ch` name, `num` number, `mhz` downlink ("?" = NAN), `slot`, `up`
uplink (only when known), `target`, `group` / `unit`, `source`, `flags`
(e encrypted, m emergency, d data, u update; "-" none), `decoder`, `demod`,
`bw` (Hz), `opt.<key>` per option, `state` ACTIVE / ENDED / RELEASED. A line
identical to one printed less than 5 s earlier is not printed again (a grant
update every second for a minute prints once; `--verbose` prints every one),
and the summary has every count:

```
[    0.300 s] (system) key="BEE00.14C" name="P25 BEE00.14C" site="RFSS 5 site 9" control=1
[    0.300 s] (channel) ch="1-0400" num=400 mhz=851.012500 slot=-1 up=806.012500
[    0.300 s] (grant) ch="1-0412" num=412 mhz=851.162500 slot=-1 up=806.162500 target="1201" group source="2010621" flags=- decoder="" demod="" bw=0 opt.nac="293"
[    0.800 s] (grant) ch="1-0412" num=412 mhz=851.162500 slot=-1 up=806.162500 target="1201" group source="2010621" flags=u decoder="" demod="" bw=0 opt.nac="293"
[    0.800 s] (call) state=ACTIVE target="1201" group source="" slot=0 flags=-
[    1.300 s] (call) state=ACTIVE target="1201" group source="2010621" slot=0 flags=-
[    1.800 s] (grant) ch="2-0007" num=7 mhz=851.250000 slot=1 target="2010999" unit source="2010621" flags=e decoder="" demod="" bw=0 opt.slot="1" opt.nac="293"
[    3.300 s] (call) state=ENDED target="1201" group source="2010621" slot=0 flags=-
[    4.300 s] (grant end) ch="1-0412" num=412 mhz=851.162500 slot=-1 up=806.162500
[    4.300 s] (call) state=RELEASED target="1201" group source="2010621" slot=0 flags=-
...
trunk system: key="BEE00.14C" name="P25 BEE00.14C" site="RFSS 5 site 9" control=1 (12 reports)
trunk channels: 1
  ch="1-0400" num=400 mhz=851.012500 slot=-1 up=806.012500
trunk grants: 9 messages (7 updates, 0 data, 1 encrypted, 0 emergency), 2 distinct channel+target
  ch="1-0412" num=412 mhz=851.162500 slot=-1 up=806.162500 target="1201" group source="2010621" flags=u decoder="" demod="" bw=0 opt.nac="293"  0.300 .. 3.800 s  8 messages
  ch="2-0007" num=7 mhz=851.250000 slot=1 target="2010999" unit source="2010621" flags=e decoder="" demod="" bw=0 opt.slot="1" opt.nac="293"  1.800 .. 1.800 s  1 messages
trunk grant ends: 1 messages, 1 channels
  ch="1-0412" num=412 mhz=851.162500 slot=-1 up=806.162500  1 messages
trunk calls: 7 messages, 1 calls (0 still active, 0 ended, 1 released)
  slot=0 target="1201" group source="2010621" flags=-  0.800 .. 4.300 s  RELEASED
trunk lines not printed (repeated within 5 s; --verbose prints them): 20
```

Further summary lines: `trunk control channel: ...` (the last
`control_channel()`), `(trunk dropped)` lines and `trunk fields cut /
dropped by the wire limits: N, messages dropped: M` when the limits changed
something (fix that in the plugin: kraken_doa does the same). The summary's
calls are counted as the host would: per slot, an ACTIVE with another known
target or source is a new call; a RELEASED after an ENDED releases that
call. The live round trip is checked with `tools/digi_test.cpp` (kraken_doa_v2),
which prints every report the real engine hands over (`[trunk] <plugin> GRANT
t=... ch=...`) and the engine's RESETs (`--retune S:HZ`, `--stop S`).

**Testing a trunking plugin** (besides *ID separation* below): its
generator writes a control channel (system information, channel
announcements, grants and grant updates timed BEFORE each call) and traffic
channel calls; the offline test of the control channel must give the
`(system)` / `(grant)` lines - every channel name, frequency, target and
option the generator put in - and of each traffic channel the `(call)`
lines, ACTIVE to ENDED / RELEASED, besides its talkers. Then a WIDE
debug-playback entry (*Test transmissions* below): the control channel AND
calls on traffic channels several hundred kHz away inside one 2.4 MHz
capture, each transmitter at its own bearing, made with
`sdk/tools/wide_scene.py`. (In a real system the traffic downlinks come
from the site, at its bearing; a scene may give each call its own bearing
to check that each VFO's DoA comes from its own channel.)

## ID separation (required when the signal has per-transmitter IDs)

If the signal carries a coded ID per TRANSMITTER - a callsign, MMSI, ICAO
address, unit ID, serial number, cell / site identity, Morse ident, device
code - the decoder MUST report it as a talker (above), so every transmitter
gets its own bearing on the 🗺 Map's 📡 DF panel instead of one bearing
mixed from all of them. Every new decoder of such a signal does this.

- **No coded ID?** A transmitter with a SIGNATURE of its own is one too: the
  radar plugin tells radars apart by their pulses and PRI pattern (id =
  frequency to 0.1 MHz) and reports only the samples that surely come
  straight from it (its main beam pointing here, not the reflections it
  lights). ssr1030 combines both: an interrogator's code where its frames
  carry a checkable one (an SSR's II / SI from its all-calls, an ACAS's
  address from its broadcasts), else its signature (a periodic all-call
  train, a non-rotating one's steady level, whisper-shout).
- **Who transmits.** The ID must name the RF transmitter of THESE samples,
  not a sender further back or a recipient: an APRS frame repeated by a
  digipeater is the digipeater's (the last path entry marked repeated - "*";
  an alias-only repeat names nobody: no talker), a repeater output carries
  every radio at the repeater's bearing, a pager's RIC / capcode is the
  recipient (POCSAG / FLEX: no talker). A downlink's base station is its cell / site (TETRA
  MCC-MNC + colour code, the MPT1327 SYS); a signal with no ID of its own
  (iMet-4 sondes) gets none - one talker would merge every such transmitter.
- **Packets** (`t.packet = true`) for bursts: one call per frame that passed
  its checks, `start_s` / `end_s` = its samples (`host.time()` of the block +
  index / rate; correct for your filters' delays). kraken_doa takes packets
  up to 0.25 s: report <= 0.24 s of a longer frame - the part that is surely
  the transmitter's (from the sync on: radiosonde; back from the decoded end:
  APRS). Lib plugins (`plugins/lib`): `RxContext::packet` with receiver
  sample indexes (`dig::Bridge::clock(rate)` maps them; tetra, mpt1327).
- **Transmissions** (`host.talker` / `talker_end`) for voice calls: the
  frame boundaries of the call, a new id ending the previous one. Also for
  an analog transmitter named by its radio ID or sub-audible tone
  (analog_id: `MDC 1234` / `CTCSS 88.5` from the carrier's / tone's onset to
  its reverse burst / end) - such a plugin decodes no voice (`.voice`
  false), so Voice to Text transcribes the VFO's analog audio and names
  each transcript after its talker. A talker whose ID is only known later
  (a post-ID at the end of the transmission) can be reported then, from the
  transmission's start: the host keeps DoA frames no span claims for 6 s
  (`FRAME_WAIT_S`), so report it within ~5 s of the start.
- **The channel**: `freq_hz` / `bw_hz` of the transmitter inside the VFO
  (Hz from your input's 0 Hz - after the host's AFC, so a decoder whose
  `freq_error` centres the signal gives 0) - kraken_doa filters exactly that
  out of every antenna first, so other signals in the VFO (neighbouring
  channels, a wider VFO than needed) don't pull the bearing. Give it always.
- **Overlapping on one channel?** kraken_doa drops packets of two talkers
  that overlap in time on the same channel (neither holds one transmitter
  alone). A spread-spectrum signal can do better: separate by CODE in the
  plugin (`multichannel` + `t.cov`, above - the lora plugin: every decoded
  symbol de-spread with its own chirp on every antenna).
- **`avg_s`** (packet averaging): 1.5 s default for fast movers; fixed
  transmitters 10 s+ (sites, navaids), slow / rare ones longer (AIS 20 s,
  APRS 30 s). **`keep_s`**: how long the talker stays listed after its last
  packet - as long as the transmitter reports rarely (ISM 24 h, sondes 3 h,
  APRS 1 h, sites 30 min).
- `t.label`: short, what the user recognises next to the ID (type, talkgroup,
  altitude, location area). Use the SAME id as your map points / table rows:
  the DF panel then shows distance and the DF error against the reported
  position, and a map popup offers its DoA.
- **UI**: a new plugin's talkers show as plain "talkers"; give them a kind
  in kraken_doa.html (`RDF_PK_KIND` plugin -> kind, `rdfIcon`, `RDF_PK_UNIT`,
  `RDF_POS_KINDS` when they report positions, `RDF_SORT_DEF`, the list
  heading in `rdfRenderPanel`).

**Test it** - every decoder with ID separation needs a multi-transmitter test
at KNOWN bearings:

1. A generator in `plugins/<id>/tools/` writes one transmitter's signal
   (real framing / FEC / CRC, its ID, start times) as cf32 at the plugin's
   rate; real recordings work too (radiosonde_auto_rx / dsd samples).
2. `python3 plugins/sdk/tools/uca_scene.py SCENE.json OUT.cf32` puts each
   transmitter at its own bearing (and frequency, time, level) on
   kraken_doa's UCA (clockwise, ANT0 on +x), with noise - several at once
   where the protocol allows it (AIS A / B, ISM).
3. `plugins/<id>/build/decoder --file OUT.cf32 --channels 5 --uca 0.12`:
   antenna 0 is decoded as live, and the summary gives each talker's
   bearing from its own samples of every antenna, with kraken_doa's rules
   (packets > 0.25 s and same-channel overlaps dropped). Each must match.
4. End to end: `uca_scene.py ... --wide OUT.cu8 --wide-offset HZ` writes the
   scene at 2.4 MHz, 8 bit, for a fake heimdall feeding the real kraken_doa
   (8091 format); the DF status (`{"rdf"}` talkers, `doa`) must give the
   same bearings.

Examples: `aprs/tools/aprs_gen.py`, `ais/tools/ais_gen.py`,
`dstar/tools/dstar_gen.py`, `nxdn/tools/nxdn_gen.py`, `ysf/tools/ysf_gen.py`, `tetra/tools/tetra_gen.py`,
`mpt1327/tools/mpt1327_gen.py`, `ils/tools/ils_gen.py`,
`pagers/tools/pocsag_gen.py`, `pagers/tools/flex_gen.py`, `v23_telemetry/tools/v23_gen.py`,
`p25/tools/p25_gen.py`, `p25/tools/p25p2_gen.py` (P25 Phase 2: a site's
TDMA channel, or one radio's inbound bursts - several radios placed by
uca_scene.py), `dmr/tools/dmr_gen.py`,
`radiosonde/tools/sonde_gen.py`; a 2.4 MHz
scene of several transmitters is too big for uca_scene.py's in-memory
mixing (5 antennas x 12 s = 2.3 GB complex128) - narrowband transmitters
spread over the 2.4 MHz go through `sdk/tools/wide_scene.py` (in 0.1 s
blocks, below), while signals made at 2.4 MHz are written by their
generators: `radar/tools/radar_gen.py`
writes its scene at known bearings itself, in 0.1 s pieces (so do
`ssr1030/tools/ssr_gen.py`, `uat/tools/uat_gen.py`, `adsb/tools/adsb_gen.py`,
`dme/tools/dme_gen.py`, `ism/tools/ism_gen.py`,
`flarm/tools/flarm_gen.py` - the last two with simultaneous packets on
different channels of one VFO - `rds/tools/rds_gen.py`: FM stations,
also at 480 kHz for an offline test - and `dab/tools/dab_gen.cpp`: a DAB
single frequency network, every transmitter with its TII code, delay and
level).

**Who transmits, again**: a message ABOUT a transmitter isn't always FROM
it - UAT's TIS-B / ADS-R messages describe aircraft but are sent by a
ground station (the uat plugin makes them the station's packets), an OGN
packet with a relay count was re-sent by another tracker (flarm: no
talker). And a signal whose payload is encrypted with unpublished keys
(FLARM) still names its transmitter in the clear header: report the ID and
its bearing, decode nothing else.

## Test transmissions for the debug playback (required with a generator)

kraken_doa's sidebar box **🧪 Debug Playback** plays a decoder's synthetic
multi-antenna test transmission through the REAL pipeline as if heimdall
had received it (spectrum, waterfall, VFO, decoder, DoA, the 📡 DF panel)
and checks the bearings. Every decoder with a generator registers its test
transmissions in **`plugins/<id>/tools/testsignals.json`** - a JSON list;
the folder carries it along when the plugin moves. Nothing large is
committed: the user presses Generate, kraken_doa runs
`sdk/tools/make_testsignal.py <id-of-entry>` (nice 10) and keeps the
result in `testsignal_cache/` (cwd-relative, gitignored, at most 3 GB - the
least recently played go).

```json
[{
 "id": "aprs-three-stations",            [a-z0-9-], unique over all plugins
 "title": "APRS: two stations and a digipeater",
 "description": "What is in it + what to expect (IDs, bearings, facts)",
 "rf_hz": 144.575e6,                     the band's centre = where the VFO goes
 "tuner_offset_hz": 300000,              the band's place in the tuner's 2.4 MHz
                                         (keeps it off DC); 0 for fixed-frequency
                                         and lock_vfo decoders
 "seconds": 16,                          loop length (keep it short)
 "vfo": {"bw_khz": 20, "demod": "NBFM", "opts": {"key": "value"}},
                                         the VFO the playback sets up (decoder =
                                         this plugin, opts = its options)
 "commands": ["..."],                    optional extra WebSocket commands
 "steps": [["{tools}/aprs_gen.py", "a.cf32", "--at", "0.5", "SRC>DST:..."]],
 "scene": {"rate": 48000, "snr_db": 20, "seed": 3, "sources": [
   {"name": "ZL1AAA-9", "file": "a.cf32", "rate": 48000, "at": 0, "bearing": 40}]},
 "expect": [{"tid": "ZL1AAA-9", "bearing": 40}],
                                         also {"bearing": B} (any talker),
                                         {"vfo": 0, "bearing": B} (the VFO's own
                                         DoA), {"event": "text"} (decoded)
 "tol_deg": 5
}]
```

- NARROW signals (a VFO of <= ~1 MHz): `steps` write one file per
  transmitter (your generator), `scene` is a `sdk/tools/uca_scene.py`
  SCENE (channels 5, radius 0.12, rf_hz and seconds default to the
  entry's). It is built with `--clean`: no noise in the file, which stays
  at the scene's low rate (`rate` must divide 2.4 MHz: 48 / 96 / 100 / 120 /
  240 kHz ...; content within +-0.35 x rate). kraken_doa interpolates it to
  2.4 MHz live, puts it at `tuner_offset_hz` and adds the noise of
  `snr_db` over the whole band - 40 bytes per sample (5 antennas), so 16 s
  at 48 kHz = 31 MB
- WIDE signals (2.4 MHz): a step writes the whole multi-antenna scene at
  2.4 MHz itself (cu8, or cf32 - converted to cu8) + its `.json` sidecar;
  name it in `"output": "out.cu8"` instead of `scene`. 24 MB per second
  with 5 antennas: a few seconds, looped (a scan / repetition period that
  fits the loop helps). `"files": {"scene.json": {"$from":
  "{tools}/scene_x.json", "seconds": 6, "station": "{station}"}}` writes an
  in-repo scene with some keys replaced
- WIDE scenes of NARROW transmitters (a trunked system's control channel and
  its traffic channels, neighbouring channels, several systems): your
  generator writes each transmitter as a narrowband file (cf32 at 48 / 72 /
  96 kHz ...), and the last step `["{sdk}/wide_scene.py", "scene.json",
  "out.cu8"]` places them anywhere in the 2.4 MHz - each with its own
  `offset_hz`, start, bearing and level, a `loop` for a control channel
  that runs the whole scene - adds noise over the whole band and writes the
  5-antenna cu8 + `out.json` the entry's `"output"` takes (see *A wide
  trunking scene* below)

**`sdk/tools/wide_scene.py SCENE.json OUT.cu8`.** SCENE: `rf_hz` (the 2.4
MHz's centre = the tuner; each source's wavelength is taken at its own
frequency), `channels` (5), `radius_m` (0.12), `seconds`, `snr_db` (a
source at gain_db 0 against the noise in `snr_bw_hz`, default 48 kHz),
`seed`, `rms` (the 8-bit level of noise + every source at once, default
0.15 of full scale, like heimdall's), `sources`: `name`, `file` (cf32 / cu8 /
cs16 IQ, or `"format": "disc_s16"` + `dev_hz` / `rms_hz` / `invert` as in
uca_scene.py), `rate` (any ratio of 2.4 MHz with a denominator <= 1000),
`from` / `dur` (the part of the file), `loop` (repeat it to the scene's
end), `at` (s), `offset_hz` (from the centre), `bearing` (array-relative,
CCW from ANT0), `gain_db`, `shift_hz` (an extra carrier error). Each source
is scaled to unit power over the part on the air. It works in 0.1 s blocks
(sources read through memory maps), so memory stays ~140 MB whatever the
length: a 20 s scene of four 48 kHz sources took 5 s and 139 MB on a
desktop x86 core (0.25 x real time; a Pi is a few times slower). Check it
offline - the sidecar gives the rate and the RF, `--offset` = the source's
`offset_hz`: `decoder --file out.cu8 --channels 5 --uca 0.12 --offset
-400000`. A playback entry's `rf_hz` is where VFO 0 goes (the control
channel) and `tuner_offset_hz` its offset from the scene's centre, so the
scene's `rf_hz` = the entry's `rf_hz` - `tuner_offset_hz`.

**A wide trunking scene** (the generator's options are the plugin's own;
`commands` may carry the trunk manager's WebSocket commands such as
`TRUNK_FOLLOW:1` - they are documented in
`kraken_doa_v2/docs/dev/trunk-following.md`):

```json
[{
 "id": "p25-trunk-two-calls",
 "title": "P25 trunking: a control channel and two calls on traffic channels",
 "description": "Site BEE00.14C RFSS 5 site 9: control channel 1-0400 on 851.0125 MHz (bearing 300 deg), grants for TG 1201 on 1-0412 (850.5125 MHz, 2.0 s) and TG 1305 on 1-0444 (850.9125 MHz, 9.0 s), each repeated as grant updates during its call; the calls' traffic channels at 40 and 200 deg. Expect (system) / (grant) on VFO 0, a voice VFO per call, talkers 2010621 at 40 deg and 2010622 at 200 deg.",
 "rf_hz": 851012500.0,
 "tuner_offset_hz": 300000,
 "seconds": 20,
 "vfo": {"bw_khz": 12.5},
 "commands": ["TRUNK_FOLLOW:1"],
 "files": {"scene.json": {"rf_hz": 850712500.0, "seconds": 20, "snr_db": 20, "seed": 5, "sources": [
   {"name": "site", "file": "cc.cf32", "rate": 48000, "loop": true, "at": 0, "offset_hz": 300000, "bearing": 300},
   {"name": "2010621", "file": "call1.cf32", "rate": 48000, "at": 0, "offset_hz": -200000, "bearing": 40},
   {"name": "2010622", "file": "call2.cf32", "rate": 48000, "at": 0, "offset_hz": 200000, "bearing": 200, "gain_db": -3}]}},
 "steps": [
  ["{tools}/p25_gen.py", "cc.cf32", "--control", "--wacn", "BEE00", "--sysid", "14C", "--seconds", "20",
   "--grant", "2.0:1-0412:1201:2010621", "--grant", "9.0:1-0444:1305:2010622"],
  ["{tools}/p25_gen.py", "call1.cf32", "--at", "2.3", "--dur", "4", "--src", "2010621", "--tg", "1201"],
  ["{tools}/p25_gen.py", "call2.cf32", "--at", "9.3", "--dur", "5", "--src", "2010622", "--tg", "1305"],
  ["{sdk}/wide_scene.py", "scene.json", "out.cu8"]
 ],
 "output": "out.cu8",
 "expect": [{"tid": "2010621", "bearing": 40}, {"tid": "2010622", "bearing": 200}],
 "tol_deg": 5
}]
```

Time the scene so every grant precedes its call (here 0.3 s), and keep
every source well inside the 2.4 MHz (`|offset_hz|` + half its width under
~1 MHz: wide_scene.py warns past the band).
- A step that is a `.py` runs with python3 -I (numpy / scipy); a `.cpp` is
  compiled once into `testsignal_cache/.bin/` - again when it or a `*.h` /
  `*.hpp` in its folder or your plugin folder is newer (`g++ -std=c++20 -O2
  -I<plugin> -Isdk -I../external/pffft ... lib/build/libkrakendig.a` - so
  a generator can use `kraken_fft.hpp`). Working directory = a scratch folder in
  the cache; placeholders `{tools}` (your tools/), `{sdk}`, `{plugins}`,
  `{work}`, `{lat}` / `{lon}` (+ `{lat+0.05}` ...) = the STATION's position
  when the user generates (0, 0 without one) and the value `"{station}"` =
  [lat, lon] - so positions for the map are near the station. Never put
  coordinates or keys in the repo
- Bearings are array-relative (as the MUSIC plot shows them, CCW from
  ANT0), like everywhere in the scenes. `expect`: per transmitter its
  talker `tid` (exact) and bearing, or only a bearing (any talker within
  tolerance - radars, DTV paths); `tol` per row overrides `tol_deg`
  (default 5). A decoder whose signal names no transmitter (pagers,
  v23_telemetry) checks `{"vfo": 0, "bearing": B}` - the VFO's own DoA
  (its MUSIC plot; put `DEC_SQUELCH_METHOD:0:DIGITAL` +
  `DEC_SQUELCH_ENABLE:0:1` in `commands`, so it comes from the decoded
  frames only and holds between them - an interferer in the gaps then
  can't move it) - and `{"event": "text"}`: an event of VFO 0's decoder
  containing the text (only events since the playback set the decoder).
  The UI's Check turns each row green when the DF status / DoA / event log
  has it; use the same numbers your README reports
- Test it: `python3 sdk/tools/make_testsignal.py --check --plugins plugins`
  (all catalogues parse, ids unique), then
  `python3 sdk/tools/make_testsignal.py <id> --plugins plugins --cache /tmp/c`
  (prints PROGRESS lines, writes `<id>.dat` + `<id>.json`; the log of the
  steps in `<id>.log`), then play it in the UI and press nothing but ▶ Play:
  the VFO, decoder and options must come from the entry
- A playback never outruns the pipeline: on a machine too busy for 2.4
  MHz x 5 antennas in real time it plays slower (the full raw buffer
  dropped whole packets - bursts cut in pieces failed the ISM decoders'
  checks while ADS-B / DME's microsecond packets survived)
- Every shipped decoder has its generator + entries (p25, dmr and
  radiosonde, whose tests used public recordings, were the last -
  2026-10-16); a new decoder brings both

## Rules for a good decoder

1. **Report only what passed a check.** A sync word match alone is not a
   frame: verify CRC / FEC / parity before `host.valid()` and before showing
   decoded content. Random noise must produce no events.
2. **Handle polarity and offset.** FSK can arrive inverted (try both sync
   polarities, or use `kp::SyncWord::inverted_distance()`); the carrier can be
   a few hundred Hz to a few kHz off (track the discriminator's DC, or report
   `freq_error`).
3. **Be cheap.** It runs live on a Raspberry Pi next to the receiver. Avoid
   per-sample allocations, `std::map` lookups and `std::string` building in
   the sample loop. The offline test prints the decode time as a % of real
   time: aim for well under 10%.
4. **Facts for state, events for happenings.** Network/site identity,
   counters, "last message" are facts; a received message, a call start/end
   are events.
5. **No stdout, no files, no network.** A plugin only decodes.
6. **Text you show is data.** Pass decoded text through `kp::printable()`
   (control characters escaped).
7. **ID separation.** If the signal names its transmitter (a coded ID per
   transmitter), report it as a talker and test it at known bearings (see
   *ID separation* above) - a core part of every such decoder.

## Helpers (`kraken_dsp.hpp`)

- Filters: `fir_lowpass(ntaps, cutoff_hz, fs)`, `fir_rrc(sps, span, beta)`,
  `fir_gaussian(sps, span, bt)`, `fir_boxcar(n)`, `Fir<float>` /
  `Fir<kp::cf>` (`push()` one sample in, one out)
- `FmDemod(fs)`: instantaneous frequency in Hz; `Nco(hz, fs).mix(x)`:
  shifts a component at +hz to 0 Hz; `Goertzel(hz, fs, n)`: tone power
- `ClockRecovery(sps, gain)`: zero-crossing symbol timing for 2/4-level FSK
  on a filtered discriminator; `push(x)` returns true when a symbol was
  sampled (`symbol()`, `centre()` = tracked DC = carrier offset in Hz)
- Bits: `bits_to_uint`, `uint_to_bits`, `pack_bits`, `SyncWord(pattern,
  nbits)` (`push(bit)` returns the Hamming distance, `inverted_distance()`)
- CRC: `crc_bits(...)` (unpacked bits, MSB first), `crc_bytes(...)` (CRC
  catalogue parameters: width, poly, init, refin, refout, xorout)
- Codes: `BlockCode(n, k, encoder)` (nearest-codeword decoding, k <= 20),
  `poly_mod` (BCH / cyclic check bits), `conv_encode`, `viterbi_decode` (soft,
  punctured = 0)
- `printable(s)`

liquid-dsp (`#include <liquid/liquid.h>`: resamplers, `symsync`, modems,
`firfilt`, `nco`) is linked as well - built from source WITHOUT FFTW and
libfec (`kraken_doa_v2/liquid.mk`), so its FEC schemes that need libfec
(convolutional / Reed-Solomon `fec` types) fail to create: use `kp::`'s
`viterbi_decode` / `BlockCode` or `lib/dig_fec` instead; and its FFT-based
objects (`fftfilt`, `spgram`, `firpfbch*`, `ofdmframe*`, `qdetector`) run
on liquid's own, slower FFT. FFTs: `#include "kraken_fft.hpp"`
(sdk/) - pffft (vendored in `kraken_doa_v2/external/pffft`, BSD-style
licence) behind FFTW-like plans, linked through `lib/build/libkrakendig.a`
(a plugin with a licence of its own: pffft's objects directly):

```cpp
kfft_complex* in  = kfft_alloc_complex(n);   // float[2] per sample (in[i][0] re, [1] im)
kfft_complex* out = kfft_alloc_complex(n);
kfft_plan p = kfft_plan_dft_1d(n, in, out, KFFT_FORWARD);   // KFFT_BACKWARD: the inverse
kfft_execute(p);                             // out = DFT(in), natural bin order, unscaled
kfft_plan r = kfft_plan_dft_r2c_1d(n, real_in, out);         // n real -> n/2 + 1 bins
kfft_execute_dft(p, other_in, other_out);    // the plan on other arrays
kfft_destroy_plan(p); kfft_free(in); kfft_free(out);
```

The results are FFTW's (sign, order, scaling). Any size works, but pffft
does N = 2^a 3^b 5^c that are multiples of 16 (32 for r2c) directly and
every other N through Bluestein (~3 transforms of a bigger size) - pick
2/3/5-smooth sizes (`kfft_direct(n)` says). Plans are made instantly (no
measuring - FFTW_MEASURE used to take 6-23 s on the Pi 5 before a plugin
read its first sample); one plan per thread. FFTW itself is no longer
linked (it is GPL; kraken_doa_v2 avoids GPL code).

## Build and test

```bash
make -C plugins PLUGIN=mydec              # build one (or `make` in kraken_doa_v2 for all)
plugins/mydec/build/decoder --info        # what kraken_doa reads
plugins/mydec/build/decoder --file capture.cf32 [--offset HZ] [--verbose] [--audio out.wav] [--station LAT,LON] [--rf HZ] [--raw]
plugins/mydec/build/decoder --file scene.cf32 --channels 5 --uca 0.12 [--rf HZ]   # every antenna: each talker's bearing (ID separation)
plugins/dtv/build/decoder --file sfn.cf32 [--channels M] [--all] [--uca RADIUS_M] [--opt separate=1]   # array plugins
plugins/tempest/build/decoder --file screen.cf32 --plots plots.jsonl   # + the last plot of each id as JSON lines
```

The summary lists each plot id's last plot (points, x range, markers, its
highest point and, for a clickable one, the option it sets); `--plots FILE`
also writes them (x0, dx, y, markers, the click fields) one JSON object per
line.

**Windows** (MSYS2 UCRT64 shell, see ../../WINDOWS.md): the same commands;
the executable is `plugins/<id>/build/decoder.exe`. A plugin has to compile
with MinGW-w64 GCC as well: the Makefile force-includes
`kraken_doa_v2/win/posix_compat.h` (pread, strptime, setenv, ...), anything
else POSIX-only (fork, pipes, signals other than INT / TERM, `/dev/...` paths)
needs an `#ifdef _WIN32` branch, and `long` is 32-bit there (LLP64): Hz,
sample counts and times are `int64_t`, printed with `%lld` / PRId64.

**macOS** (Apple clang + Homebrew, see docs/dev/macos.md): the same
commands; the Makefile force-includes `kraken_doa_v2/mac/posix_compat.h`
(pipe2, fdatasync, `st_mtim`, std::bit_cast for older libc++ ...) and
`kraken_doa_v2/mac/fast_complex.h`, which makes `std::complex` `*`, `*=`
and `std::norm` as fast as GCC's `-fcx-fortran-rules` on Linux - write
complex arithmetic normally, no helper needed. Apple
clang 14 (macOS 12) can't capture a structured binding in a lambda - name it
in the capture (`[&key = k]`).

`--rf HZ` is the RF at the recording's centre (`host.rf_hz()` = it +
`--offset`); `ai/sigtool.py` captures carry it in their `.json` sidecar
(`rf_hz`), which is used when `--rf` isn't given.

The offline test reads complex float32 (`.cf32`, rate from the `.json`
sidecar that `ai/sigtool.py` writes, or `--rate`), rtl_sdr `.cu8`, `.cs16`
or 2-channel IQ `.wav`; mixes the signal at `--offset` Hz to the centre,
resamples to your `sample_rate` exactly like the live path and feeds it in
10 ms blocks. It prints the events with their time in the recording, then a
summary:

```
=== Pagers (POCSAG + FLEX) summary ===
input: 6.16 s, decode time 0.05 s (0.8% of real time)
VALID FRAMES: 5 (first at 1.400 s)
events: 5
facts:
  POCSAG                 2400 bit/s, deviation 4.5 kHz
  ...
```

Recordings: `python3 ai/sigtool.py capture --freq HZ --rate HZ --seconds S -o x.cf32`
records from the running receiver (see `ai/sigtool.py -h` for analysis
tools). Test on noise too (e.g. `--offset` to an empty part of a wide capture):
a good decoder reports nothing there.

## Moving plugins between receivers

A plugin is just its source folder: copy `plugins/<id>/` (without `build/`)
into the other receiver's `kraken_doa_v2/plugins/`, run `make` in
`kraken_doa_v2` (or `make -C plugins PLUGIN=<id>`), then press ↻ Rescan next to
"Decoder plugins" in the web UI's ⚙ Setup Settings → 🔐 Decoders (or restart). It
builds against that receiver's `sdk/` and `lib/`, so both should run the same
version of the suite.

Plugins are selected per VFO and that choice is saved with the VFO settings
(`PLUGIN:<id>`); a receiver without the plugin shows it as missing.

## Licensing a plugin

The SDK - `sdk/` and this guide - and the Windows / macOS helpers every plugin
links (`kraken_doa_v2/win/`, `kraken_doa_v2/mac/`) are under the MIT License (`sdk/LICENSE`), so a plugin
can be under any licence. A plugin folder WITHOUT a `LICENSE` file is part of
the KrakenSDR DoA software (KrakenSDR Source Available Licence,
`kraken_doa_v2/LICENSE`). Put a `LICENSE` file in it (and an SPDX line in
each of its files, e.g. `// SPDX-License-Identifier: GPL-2.0-or-later`) and
the plugin is under that licence instead - `plugins/ism` is GPL-2.0-or-later,
because it follows rtl_433's device decoders; `plugins/flarm` is GPL-3.0,
because its OGN parity-check matrix comes from the OGN project's GPL-3.0
diy-tracker.

kraken_doa runs every plugin as a process of its own and talks to it only
through the pipe protocol in `kraken_plugin.hpp`, so a plugin with its own
licence is a separate program. It is built only from:

- its own files,
- the SDK (`kraken_plugin.hpp`, `kraken_dsp.hpp`, `kraken_fft.hpp`,
  `plugin_host.cpp`) and the Windows helpers - MIT,
- pffft (BSD-style, through `kraken_fft.hpp`) and liquid-dsp (MIT).

`plugins/Makefile` enforces this: such a plugin is compiled without `-Ilib` /
`-I../include` and linked with pffft's objects instead of
`lib/build/libkrakendig.a`, and the build stops when one of its sources
includes a header from anywhere else (`../lib/...` too). Its generators in
`tools/` are built the same way (`make_testsignal.py`, the Windows prebuild).
So it can't use `lib/` (dig_common, dig_fec, dig_fsk4, the vocoders ...) -
and don't copy code from there or from kraken_doa either: that code is under
the source-available licence. The other way round, code taken or translated
from a GPL / LGPL project belongs ONLY in such a plugin, under a licence that
project's licence allows (GPL-2.0-or-later code: GPL-2.0-or-later or
GPL-3.0-or-later), with its copyright notices kept and the files it follows
named in each file.

## The shared library (`lib/`)

The shipped protocol decoders are thin plugins around `lib/` (`dstar/decoder.cpp`
is ~50 lines). Their receivers were written against an `RxContext` /
`Report` interface; `dig::Bridge` (dig_common.hpp) maps it onto `kp::Host`,
and `dig::FmFrontEnd` (48 kHz -> 12.5 kHz channel filter -> discriminator in
Hz) / `dig::RrcFrontEnd` give them their input. Your plugin can use any of it
(`#include "dig_fec.hpp"` etc.) - unless it has a licence of its own (above):
`lib/` is under the KrakenSDR Source Available Licence.

## Example

`plugins/adsb/`: ADS-B / Mode S on 1090 MHz at 2.4 MHz - pulse-position
demodulation, CRC-24 with error repair, CPR position decoding, map points
and the station location.

`plugins/ism/`: licence-free band devices in the whole 2.4 MHz (`.lock_vfo`)
- bursts found by a short FFT per frequency (several devices at once), each
mixed down and decimated to 240 kHz, demodulated OOK / FSK,
sliced (PWM / PPM / PCM / Manchester) and handed to many small protocol
decoders registered with `ISM_PROTOCOL`; each transmission is a talker packet
at the device's own frequency (`t.freq_hz` / `t.bw_hz`) - a DoA per device.

`plugins/dme/`: DME / TACAN beacons at 2.4 MHz - `host.rf_hz()` to find the
1 MHz channels inside the VFO, an FFT channelizer, pulse / pair detection,
Morse idents (`lib/navaid.hpp`), talker packets for a DoA per beacon.
`plugins/ils/`: ILS / VOR / marker beacons - AM tone depths, the VOR radial
from two 30 Hz phases, block-wise FFT processing.

`plugins/pagers/`: pagers - `pocsag.hpp` POCSAG (2-FSK, 512/1200/2400
bit/s demodulated in parallel, BCH(31,21) correction with a parity check
against miscorrection, numeric and alphanumeric messages) and `flex.cpp`
FLEX (1600 / 3200 / 6400 bit/s, 2- and 4-level FSK, interleaved blocks,
fragmented messages; a symbol grid from the sync, timing followed per
block) on one front end (`decoder.cpp`). The protocol plugins (`p25/`,
`dmr/`, `nxdn/`, `ysf/`, `dstar/`, `tetra/`, `mpt1327/`) show more advanced
techniques: 4FSK sync correlation, soft-decision Viterbi, Reed-Solomon,
interleaving, TDMA burst handling, vocoders; `aprs/` an AFSK modem with
HDLC framing.
