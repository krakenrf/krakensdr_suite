# Decoder plugins

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Decoder plugins (`plugins/`, `src/digital/dig_plugin.cpp`):**
- A plugin is a folder `plugins/<id>/` (`[a-z0-9_-]{1,32}`, not `sdk` /
  `lib`): decoder.cpp with `KRAKEN_PLUGIN(Class, {.id, .name, .description,
  .version, .sample_rate, .min_vfo_rate, .author, .auto_detect, .options})`
  (fields in that order; `.auto_detect` is ignored), optional extra .cpp/.hpp, README.md. API
  `plugins/sdk/kraken_plugin.hpp` (`kp::Decoder::process(const kp::cf*, n)`
  gets the VFO's complex baseband at `sample_rate`; `kp::Host`: fact / event /
  valid / audio (8 kHz) + audio_channel (whose talker channel - Voice to
  Text) / voice_state / freq_error / verbose / voice_wanted /
  time / log, map_point / table_* / message / raw (data log) / talker +
  talker_end (DoA per radio, see *DoA per talker*) / station / tune
  (retune request, `Info::tunes` plugins only - see *LTE / 5G NR Cell
  Decoder*) / table_detail (a table row's label / value lines) / rf_hz +
  rf_inverted (the RF at the input's 0 Hz: wire RF 7 "rf_hz,inv" = the
  engine's `rf_hz_` + `afc_hz_` (mirrored with invert), sent by
  `send_settings` when it moves > 10 Hz and after a (re)start; offline
  `--rf HZ` or the sidecar's `rf_hz`); `kp::Option` key/label/default/choices "v=Label|..."/help),
  helpers `plugins/sdk/kraken_dsp.hpp`, the shared library `plugins/lib/`,
  guide `plugins/SDK.md` (written for LLMs too)
- `plugins/Makefile` builds `lib/build/libkrakendig.a`, then links each
  plugin's sources + `sdk/plugin_host.cpp` + the library into its OWN
  executable `plugins/<id>/build/decoder` (linked as decoder.tmp, then
  renamed: a running copy keeps its inode). The top Makefile's `plugins`
  target runs it with -k and only warns on failure; CMake has an ALL target.
  Every plugin and libkrakendig build with `-O3 -fno-math-errno
  -fcx-fortran-rules` (+ `-mcpu=native` on aarch64 - see
  docs/dev/compiler-flags.md). An optional `CXXFLAGS` file in a plugin folder adds
  compiler flags (last on the command line; no shipped plugin has one). A
  `LICENSE` file in a plugin folder = the plugin has a licence of its own:
  built from its files + the MIT SDK + pffft only, no `lib/` (see
  docs/dev/licensing.md; ism, flarm)
- `decoder --info` (JSON incl. `options` and `options_tsv` - the
  tab-separated copy kraken_doa's flat JSON reader parses; read by
  `PluginRegistry::scan()` at startup, on `PLUGINS_RESCAN` (worker thread,
  `scan_async`) and after builds; `generation()` bumps when the list
  changes), `--serve` (live), `--file REC [--offset HZ] [--opt k=v]
  [--verbose] [--audio out.wav]` (offline test: same resampling / 10 ms
  blocks as live, cf32 / cu8 / cs16 / 16+24-bit IQ WAV, prints events +
  "VALID FRAMES: N"); `--channels M --uca R` on a recording of every
  antenna (sample-major): antenna 0 decoded (multichannel plugins: all),
  then `talker_bearings` - each talker's covariance from its own samples
  of every antenna (spans 40 ms inside, packets exact, each through its
  `freq_hz` / `bw_hz` channel or the plugin's band; > 0.25 s packets and
  same-channel overlaps dropped like TalkerDoa; the resampler's delay
  taken off) -> MUSIC, one source, kraken_doa's UCA (needs --rf)
- Every antenna for `kp::Info::multichannel` plugins: `DigitalDecoder::push(..,
  mc, mc_m)` (the pipeline packs the block's channels as int16 x MC_SCALE
  only while `wants_multichannel()` and the block holds the coherent array:
  not multi-tuner, not fm_only), the Block carries them, `Engine::run`
  sends SAMPLES_MC chunks instead of SAMPLES to such a runner at the VFO's
  rate while no AFC correction is applied (invert = every antenna's Q
  negated; with one, one channel as before), same pos_map. digi_test `u8mc --channels M` feeds an interleaved recording
- Live (`Runner` in digital_decoder.cpp): the process is started on the
  decoder thread (`PluginProcess`, posix_spawn, non-blocking pipes), SAMPLES
  in 16k chunks (dropped, counted, when > 1 s behind - SAMPLES_MC /
  SAMPLES_ARRAY too; offline: blocking
  `flush_blocking`), replies polled after every block. Protocol: 8-byte
  header (type, len) + payload, types in kraken_plugin.hpp `kp::wire`
  (SYNC / SYNC_DONE drain the offline tests). The plugin's fd 1 is pointed
  at stderr (protocol on a dup), so a stray printf can't corrupt it; its
  stderr tail (30 lines) is in the status JSON (`plugins[].log`) and the
  panel's "Plugin output". Stopping a decoder closes every plugin's stdin
  first, then reaps them (parallel exit - AUTO has 6)
- No inherited fds: every child kraken_doa starts (plugins, `--info` runs,
  the Voice to Text helper, the AI bridge, curl downloads, the playback's
  generator; the tetra plugin's codec programs too) gets only its pipes /
  stdio - `spawn_fds::close_above_stdio` (`include/utils/spawn_fds.hpp`:
  `posix_spawn_file_actions_addclosefrom_np(3)` as the last file action,
  `closefrom(3)` after fork) - and kraken_doa's own sockets are created
  `SOCK_CLOEXEC` (tcp_client, gpsd, web mapper incl. accept4, geo_http; uWS's
  already were). Before (2026-10-15) a plugin held copies of the 8091 /
  8092 / gpsd sockets: heimdall kept the old 8091 connection (and queued
  data to it) after kraken_doa had reconnected
- Crash / malformed message -> event with the signal and last stderr line,
  restart after 2, 4 .. 30 s; missing / unbuilt plugin -> retried every 5 s
  (a snapshot may name a plugin this receiver lacks: accepted on replay,
  refused interactively). The exe's mtime is checked every 2 s: a rebuild
  restarts the decoder ("Rebuilt plugin loaded")
- Real-recording checks (2026-10-16, part 1 - downloads only in the job's
  scratch, reference decoders built there from source, results in each
  plugin README's Tests): uat (a CC1310 UAT capture = dump978; generator
  through dump978 48 / 48), flarm (none published - generator through
  flare's nrf905_demod: the FLARM preamble was inverted, fixed; OGN packets
  through the OGN tracker's own code), beacon406 (sigidwiki EPIRB IQ +
  F4EHY's dec406 set: the demodulator lost PDF-2 / a whole off-air burst on
  real carrier phase noise - per-bit phase pass, phase-robust sync, polarity
  from the bit sync), ils (a real VOR's radial = an independent measurement;
  its ident under voice made the 1020 Hz keyer read over 40 ms; ILS
  synthetic only), aprs (WA8LMF TNC test CD: 967 vs multimon-ng's 955 /
  660 vs 517; 17-byte AX.25 frames without a PID were dropped - fixed),
  pocsag (sigidwiki IQ + multimon-ng's samples = multimon-ng), v23 (its
  real recordings are gone, none public), generators of adsb (dump1090:
  every one of its 217 messages on its own capture + 111; of the
  generator's 197 it reads 144, all byte-identical), ais (AIS-catcher 7 / 7), radiosonde (rs1729 12 / 12
  RS41 + M20), dme (pulse shape = a real 40X)
- Real-recording checks, part 2 (2026-10-16): ctcss (sigidwiki CTCSS IQ:
  173.79 Hz = an independent tracker; a reverse burst's end had been moved
  0.5 s back to a carrier fade taken for a capture - fixed; the "DCS" IQ
  there is a 150 bit/s 21-bit code: nothing, correct; DCS synthetic only),
  wildlife (UAV-RT's published drone IQ of a real tag: found at 3x its
  period then "faster x3.0" - impulse blips made its pulses "groups", and a
  multiple-period start is now corrected; generator `chirp_hz`), lora
  (21 real Meshtastic packets from the public MQTT, real header +
  ciphertext modulated: the AES-CTR counter was little-endian in plugin AND
  generator - only 16 bytes decrypted; 11 -> 21 / 21; real Meshtastic IQ
  none), generators through dsdcc (DMR repeater IDs / CC, D-STAR headers,
  NXDN unit / TG exact; DMR direct mode and P25 not checkable there), p25 /
  tetra on sigidwiki IQ (WACN / system = the file names; MCC 228 / MNC
  8889), mpt1327 (the public "MPT1327-like" / taxi files aren't), ism
  generator through rtl_433 (all fields; the WH24 frame read as a WH65B -
  its tail, fixed). Not done: radar / ssr1030 / dtv / cells generators vs
  the live captures (their generators were built from those captures'
  measurements; no independent reference)
- Performance pass (2026-10-26; a 2015 MacBook Air i5-5350U, Apple
  clang 14; Pi / Windows not measured):
  the offline host's per-talker bearings (`talker_bearings`, plugin_host.cpp)
  filter every antenna's channel at once (`fir_antennas<M>`: independent
  sums in registers, each over the taps in order, the products as before):
  `ism-six-devices` offline 10.8 -> 5.7 s, bearings identical - offline
  only. All 37 test transmissions decode identically (bar timing lines);
  offline user CPU of 36 of them (rds aside: offline it runs at 2.4 MHz)
  41.4 -> 33.9 s (-18 %, medians of 3 interleaved; job scratch
  `plugins/scripts/ab.py`).
  Tested, not taken: the live host's SAMPLES_MC deinterleave with the
  antenna count as a template / converted in one pass then gathered
  (~3 ns per antenna sample either way, within the noise). Measured with
  `-ffp-contract=fast`: the micro-benchmarks' speed within +-10 % of the
  default (clang contracts within a statement), adsb's and lora's new
  loops then no longer bit-identical to the old ones (rounding); HEAD and
  the new plugins both built with it decode the 11 test transmissions of
  adsb, lora, ism, dtv, p25 and dmr identically (the flag itself moves one
  lora "peak/mean" figure, 1994 -> 1998)
