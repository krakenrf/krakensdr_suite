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
│   └── plugin_host.cpp   main() of every plugin (live + offline test modes)
├── lib/                  shared digital-radio library (libkrakendig.a, linked into
│                         every plugin; only what a plugin uses ends up in it):
│                         dig_fec (Golay/Hamming/BCH/RS/Viterbi/CRCs), dig_fsk4
│                         (P25/DMR 4FSK sync), dig_vocoder (IMBE, mbelib AMBE,
│                         TETRA ACELP), dig_common (front ends, the Bridge below)
├── p25/ dmr/ tetra/ dstar/ nxdn/ mpt1327/   the shipped protocol decoders
├── pocsag/ aprs/         more shipped decoders (APRS was written by the AI lab)
├── adsb/                 ADS-B / Mode S at 2.4 MHz - an example of map points
└── <id>/                 ONE plugin = one folder, named by its id
    ├── decoder.cpp       required: the decoder class + KRAKEN_PLUGIN(...)
    ├── *.cpp / *.hpp     optional extra sources (all .cpp files are compiled)
    ├── README.md         what it decodes, sources/specs used, test results
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
                          .talkers = false})              // true: reports who transmits (host.talker)
```

The fields of `kp::Info` must be given **in this order** (C++20 designated
initializers); any of them can be left out.

**Options** (`kp::Option`: key, label, default, choices, help) are the
decoder's own settings. The panel shows them while the plugin is selected (in
Auto detect: once it has locked onto this plugin) - `choices` = `"value=Label|..."`
makes a list, `""` a text field. They arrive through `option(key, value)`:
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
| `host.audio(pcm, n)` | decoded audio, 8 kHz mono float (+-1). Only played when the user listens (Demod "Digital"); skip the work when `!host.voice_wanted()` |
| `host.voice_state(s)` | short call status shown next to the Listen button |
| `host.freq_error(hz)` | measured carrier offset (+ = above the VFO centre): the host slowly re-centres the input. Optional |
| `host.verbose()` | the panel's "Log all messages" option |
| `host.time()` | seconds of input processed so far (sample clock) - use it for timeouts, not the wall clock |
| `host.log(text)` | debug output (stderr) |
| `host.map_point(p)` | a position on the web UI's 🗺 Map (`kp::MapPoint`, below) |
| `host.map_remove(id)` | takes a point off the map |
| `host.station(&lat, &lon)` | the receiver's location (sidebar → Station Information), false if it has none |
| `host.table_columns(cols)` | headings of a live table in the panel (e.g. the aircraft ADS-B hears) |
| `host.table_row(key, cells)` | adds / updates (same key) a table row; rows not updated for 10 min drop out |
| `host.table_remove(key)` | removes a table row |
| `host.message(from, text)` | a free-text message the decoder received (a pager text): kraken_doa looks for a street address in it and shows it as an incident (set `.messages = true`) |
| `host.raw_wanted()` / `host.raw(text)` | one raw frame as text for the decoder data log (only while `raw_wanted()` - the user ticked "Raw frames") |
| `host.talker(t)` / `host.talker_end(at_s, channel)` | who is transmitting (`kp::Talker`: radio ID, label, start / confirmed end in `host.time()`, channel, packet): the DoA is split per radio / aircraft (set `.talkers = true`, below) |

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
p.kind = "aircraft";        // aircraft | vehicle | ship | person | station | point
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
No `talker_end`. The adsb plugin is the reference (`talker_packet`); its
start times are within half a sample of the true ones. Two optional packet
fields: `t.freq_hz` / `t.bw_hz` = the packet's channel inside your band
(Hz from what you receive at 0 Hz) - kraken_doa then filters that channel
out of every antenna first, so talkers on neighbouring channels of the
same VFO don't mix (the ais plugin: -25 / +25 kHz, 16 kHz wide); `t.avg_s`
= how long to average a talker's packets (default 1.5 s for fast movers
that send often; ais: 20 s). The offline test
counts them ("talker packets:", each one with `--verbose`).

**Tables.** A decoder that tracks many things at once (aircraft, stations,
radios) can show them as a table above its facts: `table_columns()` once
(the constructor), then `table_row(key, cells)` whenever an entry changes -
at most every second or so per row; the page sorts by any column and adds a
"Seen" column (time since the row's last update). Use the same key as the
entry's map point: clicking the row then shows it on the map. kraken_doa
empties the rows when the VFO is retuned or the decoder switched. The
offline test prints the final table.

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
plugins/mydec/build/decoder --file capture.cf32 [--offset HZ] [--verbose] [--audio out.wav] [--station LAT,LON] [--raw]
```

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

`plugins/pocsag/decoder.cpp`: POCSAG pagers (2-FSK, 512/1200/2400 bit/s
demodulated in parallel, BCH(31,21) correction with a parity check against
miscorrection, numeric and alphanumeric messages). The protocol plugins (`p25/`,
`dmr/`, `nxdn/`, `dstar/`, `tetra/`, `mpt1327/`) show more advanced
techniques: 4FSK sync correlation, soft-decision Viterbi, Reed-Solomon,
interleaving, TDMA burst handling, vocoders; `aprs/` an AFSK modem with
HDLC framing.
