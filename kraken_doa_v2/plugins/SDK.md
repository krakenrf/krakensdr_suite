# KrakenSDR decoder plugins: SDK

Every decoder in the receiver's **Digital Decoder** (sidebar → 🔐 Digital
Decoder → Mode) is a plugin - the protocols that ship with the suite (P25,
DMR, TETRA, D-STAR, NXDN, MPT1327, POCSAG, APRS) as well as the ones you add.
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
                                       "help text"}}})    // settings in the panel
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
runs side by side; the one whose `valid()` frames dominate is shown, and only
its voice / carrier offset is used. So a plugin must never call `valid()` on
noise or on other protocols - it would steal the detection. (`Info::auto_detect`
is deprecated and ignored; it is only kept so older plugins compile.)

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
plugins/mydec/build/decoder --file capture.cf32 [--offset HZ] [--verbose] [--audio out.wav]
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

`plugins/pocsag/decoder.cpp`: POCSAG pagers (2-FSK, 512/1200/2400 bit/s
demodulated in parallel, BCH(31,21) correction with a parity check against
miscorrection, numeric and alphanumeric messages). The protocol plugins (`p25/`,
`dmr/`, `nxdn/`, `dstar/`, `tetra/`, `mpt1327/`) show more advanced
techniques: 4FSK sync correlation, soft-decision Viterbi, Reed-Solomon,
interleaving, TDMA burst handling, vocoders; `aprs/` an AFSK modem with
HDLC framing.
