# KrakenSDR decoder plugins: SDK

Every decoder in the receiver's **Digital Decoder** (sidebar → 🔐 Digital
Decoder → Mode) is a plugin - the protocols that ship with the suite (P25,
DMR, TETRA, D-STAR, NXDN, MPT1327, POCSAG, APRS, ADS-B) as well as the ones you add.
Plugins are written by hand or by the **🤖 AI Signal Lab**, which has an LLM
agent write, build and test one for a signal it investigated.

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
│       tools/make_testsignal.py  builds a <id>/tools/testsignals.json entry for the debug playback
├── lib/                  shared digital-radio library (libkrakendig.a, linked into
│                         every plugin; only what a plugin uses ends up in it):
│                         dig_fec (Golay/Hamming/BCH/RS/Viterbi/CRCs), dig_fsk4
│                         (P25/DMR 4FSK sync), dig_vocoder (IMBE, mbelib AMBE,
│                         TETRA ACELP), dig_common (front ends, the Bridge below),
│                         radar_pri / radar_scan (PRI patterns, antenna scan / main
│                         beam - radar, ssr1030)
├── p25/ dmr/ tetra/ dstar/ nxdn/ mpt1327/   the shipped protocol decoders
├── pocsag/ aprs/         more shipped decoders (APRS was written by the AI lab)
├── adsb/                 ADS-B / Mode S at 2.4 MHz - an example of map points
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
├── uat/                  978 MHz UAT (US ADS-B + ground uplink): CPFSK at 2.3 samples per bit,
│                         Reed-Solomon GF(256), FIS-B headers / DLAC text; TIS-B = the ground station's
├── flarm/                FLARM (ID + bearing only: encrypted payload) / OGN trackers (LDPC soft
│                         decoding, TEA de-whitening, stealth honoured) on every channel in the 2.4 MHz
├── ctcss/                CTCSS tones / DCS codes under analog FM (FFT search + narrowband tracker,
│                         Golay DCS words): a transmission talker per tone, tone squelch, no voice
├── beacon406/            406 MHz distress beacons (Cospas-Sarsat): bursts on every channel of a 100 kHz
│                         VFO, biphase-L demodulator, BCH syndrome tables, every protocol; a packet
│                         talker per 15-hex ID on its own channel; distress vs self-test
└── <id>/                 ONE plugin = one folder, named by its id
    ├── decoder.cpp       required: the decoder class + KRAKEN_PLUGIN(...)
    ├── *.cpp / *.hpp     optional extra sources (all .cpp files are compiled)
    ├── CXXFLAGS          optional: extra compiler flags for this plugin (e.g. -O3)
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
                          .description = "one line",      // the panel's help text
                          .version = "1.0",
                          .sample_rate = 48000,           // rate process() receives
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
                          .squelch_hold_s = 0})           // > 0: the Digital squelch's hold (below)
```

The fields of `kp::Info` must be given **in this order** (C++20 designated
initializers); any of them can be left out.

**Options** (`kp::Option`: key, label, default, choices, help) are the
decoder's own settings. The panel shows them while the plugin is selected (in
Auto detect: once it has locked onto this plugin) - `choices` = `"value=Label|..."`
makes a list, `""` a text field; exactly two choices with the values `0`
and `1` (`"0=Off|1=On"`) make an on / off button ("● Label: On"; a
choice's text after its name in brackets is left off the button and shown
in its tooltip). They arrive through `option(key, value)`:
the default when the plugin starts, then whatever the user picks (`""` =
back to the default). They are saved per VFO.

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
| `host.verbose()` | the panel's "Log all messages" option |
| `host.time()` | seconds of input processed so far (sample clock) - use it for timeouts, not the wall clock |
| `host.log(text)` | debug output (stderr) |
| `host.map_point(p)` | a position on the web UI's 🗺 Map (`kp::MapPoint`, below) |
| `host.map_remove(id)` | takes a point off the map |
| `host.station(&lat, &lon)` | the receiver's location (sidebar → Station Information), false if it has none |
| `host.rf_hz()` / `host.rf_inverted()` | the radio frequency (Hz) at 0 Hz of your input - the VFO's centre plus the host's frequency correction - NAN when unknown (offline test without `--rf`); `rf_inverted()`: the input is mirrored (the "invert" option), +f is then `rf_hz() - f`. Name the channel you receive (`dme`: a DME channel from its reply frequency), or find the channels inside a wide VFO. A retune also calls `reset()` |
| `host.table_columns(cols)` | headings of a live table in the panel (e.g. the aircraft ADS-B hears) |
| `host.table_row(key, cells)` | adds / updates (same key) a table row; rows not updated for 10 min (or `table_keep`) drop out |
| `host.table_remove(key)` | removes a table row |
| `host.table_keep(seconds)` | how long a row stays without an update (60 s .. 7 days, default 10 min) - things that report rarely want long (ism: its "Keep devices" option, 24 h) |
| `host.table_detail(key, lines)` | a row's details: label / value pairs the panel shows when the row is opened (▸) |
| `host.message(from, text)` | a free-text message the decoder received (a pager text): kraken_doa looks for a street address in it and shows it as an incident (set `.messages = true`) |
| `host.raw_wanted()` / `host.raw(text)` | one raw frame as text for the decoder data log (only while `raw_wanted()` - the user ticked "Raw frames") |
| `host.talker(t)` / `host.talker_end(at_s, channel)` | who is transmitting (`kp::Talker`: radio ID, label, start / confirmed end in `host.time()`, channel, packet): the DoA is split per radio / aircraft (set `.talkers = true`, below) |
| `host.talker_forget(id)` | drops a talker now - its bearing, history and row in the 📡 DF list (wire TALKER_FORGET 33; ism: its unknown devices when "Unknown signals" is set to Ignore) |

**Voice.** Set `.voice = true` if the decoder sends audio (`host.audio`):
only then does its panel tab offer the Listen button.

**Text messages → incidents.** A decoder of text messages (pagers...) can
pass each one to `host.message(from, text)` and set `.messages = true`.
kraken_doa finds a street address in the text and looks it up online
(OpenStreetMap Nominatim) around the station; the result is an incident in
the decoder's tab and, with "Plot on map", on the 🗺 Map. The plugin needs
no map code (and must not use the network itself).

**Decoder data log.** The user can log what every decoder reports to
disk (sidebar → 🗂 Decoder Logging, one JSON Lines file per day): your
events, text messages and map points are logged without any code in the
plugin. Raw frames are the one thing to add: while `host.raw_wanted()` is
true, pass each frame that passed its checks to `host.raw(text)` - one line,
printable, compact (hex for binary frames, e.g. ADS-B `"8D4840D6... -14.8"`
= the Mode S message + level in dB; APRS the TNC2 packet; POCSAG "RIC n
F<f> <baud>" + the message codewords). Check `raw_wanted()` first: building
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
`set_talker`, `tick`); nxdn, dstar and dmr do the same. Plugins built on
`plugins/lib` get the time conversion from `dig::Bridge::talkers(rate)` +
`restart_clock()` (call it in `reset()`). The offline test prints the
transmissions it would cut ("talkers:" in the summary).

Several transmissions at once on one frequency (DMR's two timeslots): give
each its own `t.channel` (0..7) and end it with `host.talker_end(at_s,
channel)`; a new id ends only its own channel's talker. Frames holding two
channels' radios go to neither. On such a channel, report a transmission
whose radio isn't known yet with `t.id = "?"` (the dmr plugin does until
the LC names it): its samples then stay out of the other channel's talker,
and the real id later takes over from the same start. A protocol with one
transmission at a time needs neither.

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
120 MB at 2.4 MHz with 5 antennas; wider ones use the whole band); `t.avg_s`
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
pieces (ctcss: every 50 ms while the tone is there) sets
`.squelch_hold_s` (ctcss 0.35), so the squelch closes soon after the
signal ends instead of passing 1.5 s of noise after every transmission.
The shipped plugins: the lib protocols report through
`RxContext::valid_at` (`dig::report_valid(ctx, mode, start, end)` with the
receiver's own sample indexes; `Bridge::clock(rate)` + `restart_clock()`
map them - `Bridge::talkers(rate)` includes it), pocsag reports each synced
batch, aprs / v23_telemetry each frame from its length, radiosonde each
frame from its FSK receiver (`FskRx::clock`). `decoder --file` counts
them ("valid frames with their samples: N, covering X s"; each one with
`--verbose`).

**Tables.** A decoder that tracks many things at once (aircraft, stations,
radios) can show them as a table above its facts: `table_columns()` once
(the constructor), then `table_row(key, cells)` whenever an entry changes -
at most every second or so per row; the page sorts by any column and adds a
"Seen" column (time since the row's last update). Use the same key as the
entry's map point: clicking the row then shows it on the map. kraken_doa
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
previous; send at most a few a second. A table row whose key is a talker id
selects that talker the same way.

The offline test runs array plugins on multichannel recordings (cf32,
sample-major: antenna 0..m-1 of sample 0, then of sample 1, ...; `channels`
in the `.json` sidecar or `--channels M`), in bursts like the live host
(`--all` = every sample back to back); `--uca R` (metres) puts the
antennas on kraken_doa's clockwise UCA and prints each spatial talker's
MUSIC bearing at the end. `plugins/dtv/tools/sfn_gen.cpp` writes synthetic
single frequency networks in that format, `plugins/cells/tools/cell_gen.cpp`
LTE / NR cells.

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
  every radio at the repeater's bearing, a pager's RIC is the recipient
  (POCSAG: no talker). A downlink's base station is its cell / site (TETRA
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
  an analog transmitter named by its sub-audible tone (ctcss: `CTCSS 88.5`
  from the carrier's / tone's onset to its reverse burst / end) - such a
  plugin decodes no voice (`.voice` false), so Voice to Text transcribes the
  VFO's analog audio and names each transcript after its talker.
- **The channel**: `freq_hz` / `bw_hz` of the transmitter inside the VFO
  (Hz from your input's 0 Hz - after the host's AFC, so a decoder whose
  `freq_error` centres the signal gives 0) - kraken_doa filters exactly that
  out of every antenna first, so other signals in the VFO (neighbouring
  channels, a wider VFO than needed) don't pull the bearing. Give it always.
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
`dstar/tools/dstar_gen.py`, `nxdn/tools/nxdn_gen.py`, `tetra/tools/tetra_gen.py`,
`mpt1327/tools/mpt1327_gen.py`, `ils/tools/ils_gen.py`; a 2.4 MHz scene of
several transmitters is too big for uca_scene.py's in-memory mixing (5
antennas x 12 s = 2.3 GB complex128): `radar/tools/radar_gen.py` writes its
scene at known bearings itself, in 0.1 s pieces (so do
`ssr1030/tools/ssr_gen.py`, `uat/tools/uat_gen.py` and
`flarm/tools/flarm_gen.py` - the last with simultaneous packets on
different channels of one VFO).

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
- A step that is a `.py` runs with python3 -I (numpy / scipy); a `.cpp` is
  compiled once into `testsignal_cache/.bin/` (`g++ -std=c++20 -O2
  -I<plugin> -Isdk ... -lfftw3f`). Working directory = a scratch folder in
  the cache; placeholders `{tools}` (your tools/), `{sdk}`, `{plugins}`,
  `{work}`, `{lat}` / `{lon}` (+ `{lat+0.05}` ...) = the STATION's position
  when the user generates (0, 0 without one) and the value `"{station}"` =
  [lat, lon] - so positions for the map are near the station. Never put
  coordinates or keys in the repo
- Bearings are array-relative (as the MUSIC plot shows them, CCW from
  ANT0), like everywhere in the scenes. `expect`: per transmitter its
  talker `tid` (exact) and bearing, or only a bearing (any talker within
  tolerance - radars, DTV paths); `tol` per row overrides `tol_deg`
  (default 5). The UI's Check turns each row green when the DF status has
  it; use the same numbers your README reports
- Test it: `python3 sdk/tools/make_testsignal.py --check --plugins plugins`
  (all catalogues parse, ids unique), then
  `python3 sdk/tools/make_testsignal.py <id> --plugins plugins --cache /tmp/c`
  (prints PROGRESS lines, writes `<id>.dat` + `<id>.json`; the log of the
  steps in `<id>.log`), then play it in the UI and press nothing but ▶ Play:
  the VFO, decoder and options must come from the entry
- Decoders still without a generator (their tests used public recordings):
  adsb, dme, ism, p25, dmr, pocsag, radiosonde, v23_telemetry - bring one
  when you touch them

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
`firfilt`, `nco`, FEC) and FFTW3 (`<fftw3.h>`, float API `fftwf_*`) are
linked as well.

## Build and test

```bash
make -C plugins PLUGIN=mydec              # build one (or `make` in kraken_doa_v2 for all)
plugins/mydec/build/decoder --info        # what kraken_doa reads
plugins/mydec/build/decoder --file capture.cf32 [--offset HZ] [--verbose] [--audio out.wav] [--station LAT,LON] [--rf HZ] [--raw]
plugins/mydec/build/decoder --file scene.cf32 --channels 5 --uca 0.12 [--rf HZ]   # every antenna: each talker's bearing (ID separation)
plugins/dtv/build/decoder --file sfn.cf32 [--channels M] [--all] [--uca RADIUS_M] [--opt separate=1]   # array plugins
```

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
=== POCSAG summary ===
input: 6.16 s, decode time 0.05 s (0.8% of real time)
VALID FRAMES: 5 (first at 1.400 s)
events: 5
facts:
  Bit rate               2400 bit/s
  ...
```

Recordings: `python3 ai/sigtool.py capture --freq HZ --rate HZ --seconds S -o x.cf32`
records from the running receiver (see `ai/sigtool.py -h` for analysis
tools). Test on noise too (e.g. `--offset` to an empty part of a wide capture):
a good decoder reports nothing there.

## Moving plugins between receivers

A plugin is just its source folder: copy `plugins/<id>/` (without `build/`)
into the other receiver's `kraken_doa_v2/plugins/`, run `make` in
`kraken_doa_v2` (or `make -C plugins PLUGIN=<id>`), then press ↻ next to
"Decoder plugins" in the web UI's Digital Decoders box (or restart). It
builds against that receiver's `sdk/` and `lib/`, so both should run the same
version of the suite.

Plugins are selected per VFO and that choice is saved with the VFO settings
(`PLUGIN:<id>`); a receiver without the plugin shows it as missing.

## The shared library (`lib/`)

The shipped protocol decoders are thin plugins around `lib/` (`p25/decoder.cpp`
is ~50 lines). Their receivers were written against an `RxContext` /
`Report` interface; `dig::Bridge` (dig_common.hpp) maps it onto `kp::Host`,
and `dig::FmFrontEnd` (48 kHz -> 12.5 kHz channel filter -> discriminator in
Hz) / `dig::RrcFrontEnd` give them their input. Your plugin can use any of it
(`#include "dig_fec.hpp"` etc.).

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

`plugins/pocsag/decoder.cpp`: POCSAG pagers (2-FSK, 512/1200/2400 bit/s
demodulated in parallel, BCH(31,21) correction with a parity check against
miscorrection, numeric and alphanumeric messages). The protocol plugins (`p25/`,
`dmr/`, `nxdn/`, `dstar/`, `tetra/`, `mpt1327/`) show more advanced
techniques: 4FSK sync correlation, soft-decision Viterbi, Reed-Solomon,
interleaving, TDMA burst handling, vocoders; `aprs/` an AFSK modem with
HDLC framing.
