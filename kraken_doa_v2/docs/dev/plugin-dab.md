# DAB / DAB+ plugin

Not loaded automatically. *Italic* topic names refer to the other files in
this folder (index: `README.md` here, or the table in
`kraken_doa_v2/CLAUDE.md`). The user-facing description, every number and the
generator's scene format are in `plugins/dab/README.md`.

**DAB / DAB+ (`plugins/dab/`, README.md there; added 2026-11-16):**
- Plugin id `dab`, "DAB / DAB+ (digital radio)", v1.0, category Broadcast &
  cellular; 2.4 MHz, `lock_vfo` (the VFO is the tuner's band: tune to the
  block), `multichannel`, `voice`, `talkers`, `map`, `manual_only` (Auto
  detect never picks it); option `service` ("" = the lowest-SId audio
  service, a SId in hex, or part of a label), shown in the panel as a list
  of the audio services (2026-11-16, `update_choices` ->
  `kp::Host::option_choices`: value = SId, label "name · DAB+ 144k", by
  name; "" = "First audio service (name)"; a setting outside the list gets
  its own entry "plus → Kraken Plus"; sent on a change only, emptied on
  reset; the option is still declared as a text field, so an older
  kraken_doa shows that). It replaced nothing: DAB was
  removed from the old `sfn` plugin (now `dtv`) in 2026-10 to become this
  plugin (*Digital TV*). Written from EN 300 401 V2.2.1 (+ V1.4.1 for FIG
  0/22), TS 103 466, TS 102 563, TS 102 980, TS 101 756, TS 103 461 alone; no
  GPL DAB implementation was consulted (the suite's `docs/dev/licensing.md`).
  Three parts by three authors: receiver (`dab_ofdm` / `dab_fic` /
  `dab_msc` / `dab_tii` / `decoder.cpp`), audio layer (`dab_audio` +
  `audio_*`), generator (`tools/dab_gen.cpp`) - the generator written
  independently of the receiver, as its check
- NO RESAMPLER (decision): at 2.4 MHz a mode I useful symbol (1 ms) is
  exactly 2400 samples, so the 1 kHz carrier spacing is one bin of a
  2400-point FFT (2^5 3 5^2 - pffft does it). The guard (590.625), symbol
  (2990.625) and null (3112.5 samples) aren't whole, the 96 ms frame is
  (230400): each FFT window starts on a whole sample, its sub-sample offset
  is a linear phase over the carriers taken out before the differential
  demodulation; the tracked sample clock offset scales the symbol positions.
  Saves resampling 2.4 Msps x 5 antennas on a Pi; the cost is the sample
  clock's inter-carrier interference, uncorrected (40 ppm: FIC MER 29.7 dB at
  54 dB SNR, 60 dB at 0 ppm; RTL-SDRs are within a few ppm). Measured: the
  front end adds no loss (MER 10.0 / 10.1 dB at 10 dB SNR; floor 60.0 dB
  from a 2.4 MHz file, 55.7 dB from a 2.048 MHz one through the offline
  host's resampler)
- CFO INSIDE THE PLUGIN (decision): no `host.freq_error()` - an AFC
  correction would take the other antennas away (README). Fractional part:
  each window derotated with the phase running on the sample index from the
  frame start; whole kHz: a bin shift + the common phase `2 pi f (w - frame
  start) / fs`. Acquisition +-410 kHz: the phase reference's differential
  correlation repeats for shifts of 32 carriers (h pattern of table 24, 0.92
  next to 0.995), so the 12 best are told apart by the coherent impulse
  response (peak / mean 1488 against 177)
- TII -> A COVARIANCE BY CODE (the ID separation): every transmitter of the
  single frequency network sends the same signal, so the VFO's DoA mixes
  them, and (unlike DVB-T's cell ID, which every SFN transmitter shares -
  `dtv` can only separate paths by delay) DAB gives each transmitter 32
  carriers of its own in the null symbol (MainId / SubId, clause 14.8). In
  `process_multi` each confirmed code gets a packet talker per null symbol
  (id `%04X-%02d-%02d` EId-MainId-SubId, label the ensemble label, avg_s 10,
  keep_s 30 min, the null symbol's 1 ms window) with `t.cov` = sum of `v v^H`
  over its carriers that no other code detected in that symbol uses, minus
  the noise covariance of that symbol's empty carriers; sent only when the
  main eigenvalue holds >= 50 % of the raw trace and >= 80 % after the noise.
  Detection: a comb with 4 slots above threshold + a structure check (both
  carriers of each pair, 3 of 4 quarters, not a neighbour comb's copy of a
  stronger code - ICI of a 54 dB transmitter at 40 ppm lands exactly there),
  confirmed after 5 of the last 20 TII null symbols (TII has no CRC), gone
  20 s after its last. Null symbol FFT at frame start + 680 samples (late as
  the phase reference allows: on the NPO recording TII SNR 12 -> 18 dB and a
  fourth transmitter). Talkers only while the FIC decodes. `t.lat` / `t.lon`
  left empty (array calibration wants 2-60 deg elevation; a ground mast is
  near 0 deg with unknown height); a FIG 0/22 position goes to a map point
  (kind `station`, same id) instead - the DF panel's position check
- TWO TII PAIR-PHASE CONVENTIONS: EN 300 401 clause 14.8 puts the phase
  reference value phi_k of a pair's FIRST carrier on both carriers; BBC
  National DAB, DAB Melbourne 1, DAB+ Sydney 1 and RAS 12D (recordings) put
  phi_k+1 on the second - their pair phase steps were coherent only that
  way. `dab_tii.cpp` keeps both sums per transmitter and uses the one that
  agrees (`Tx::convention`; "pairs phi k/k" / "k/k+1" in the TII fact with
  `verbose=1`). Only the delay estimate depends on it (pair step coarse +
  50-sample-periodic profile fine, shown when they agree within 20 samples
  and coherence >= 0.5); the covariance (sums of `v v^H`) is blind to a
  per-carrier phase, so bearings are not. The generator got a scene key for
  it (2026-11-16): `"tii_pair_phase": "k"` (default) | `"k+1"` per
  transmitter; `scene_sfn3.json`'s 12/7 uses k+1
- UI: kind `transmitter` 🗼 (`RDF_PK_KIND` dab; list "TII / Ensemble /
  Nulls", unit "null symbol", "Every DAB transmitter", by name = TII code).
  A new kind, not rds's `broadcast` (its heads say PI / PS / Groups) or
  tetra's `site` (Area · carrier / Slots, no positions). In `RDF_POS_KINDS`
  like LoRa's `node`: only some report a position (an ensemble sending FIG
  0/22 - few do), and the Dist / Δ pos columns appear only when a listed
  talker has one; its "Plot on map" hint adds "only when the ensemble sends
  its transmitters' positions". With them the Ensemble column is left out
  (`noInfo` in `rdfRenderPanel`; the label, the same on every row plus
  " · N km", goes into the row's tooltip): with it the table was 377 px in
  the 370 px list and Nulls was cut off (end-to-end test, 2026-11-16)
- Text from the air: labels / dynamic label converted to UTF-8 (EBU Latin,
  UCS-2, UTF-8) and passed through the plugin's own `dab::printable_utf8`
  (`dab_tables.cpp`: controls and invalid bytes `\xNN`, valid multi-byte
  characters kept) - `kp::printable` (SDK) escapes every non-ASCII byte,
  which would turn "Tëst" into "T\xC3\xABst". The SDK was left as it is; a
  UTF-8-keeping variant there is a possible later change
- AUDIO LAYER (`dab::SubchannelDecoder`): logical frames of the selected
  sub-channel -> 8 kHz mono, dynamic label, facts. DAB: Layer II frames
  played only after the header check, header CRC and ScF-CRC (the ScF-CRC
  check switches itself off after 25 mismatches with good header CRCs - an
  encoder that writes none); PCM by minimp3 (vendored `minimp3.h`, CC0,
  commit ea99364; chosen after matching ffmpeg to 3e-5 on all 96 DAB rate /
  mode combinations). DAB+: Fire-code sync, RS(120,110) over the virtual
  interleaver, AU CRCs, an AudioSpecificConfig from the super frame header
  (960-sample AAC-LC, explicit hierarchical SBR / PS), concealment for bad
  AUs, mono core decoded mono / stereo downmixed in the plugin (fdk's own
  downmix is L+R and clips). PAD / DLS per EN 300 401 7.4 / TS 102 980.
  Output: Kaiser FIR + integer decimation to 8 kHz, lost frames = silence
  with 4 ms fades. Only the PCM stages wait for a listener (`voice_wanted`);
  framing, RS, CRCs and labels always run (the MSC costs 0.2-0.3 % CPU)
- CODEC POLICY: Layer II is built in (minimp3, CC0). HE-AAC v2 needs
  Fraunhofer FDK AAC (libfdk-aac, FDK licence - not GPL, but not something
  the suite ships: Debian has it in non-free), so it is the user's, like
  mbelib: `audio_aaclib.cpp` dlopens `$KRAKEN_AAC_LIB` / `libfdk-aac.so.2` /
  `/usr/local/lib/...` / `libfdk-aac.so.1` (Windows `libfdk-aac-2.dll`,
  macOS `libfdk-aac.2.dylib` + Homebrew paths) at first use, declares only
  the few API functions it calls (from the public API documentation), and
  refuses decoder modules below 2.x or a library missing a function. GPL
  decoders (faad2) are out by the licensing rules. Without it DAB+ still
  syncs and shows everything but PCM (*Digital voice*)
- Codec check (2026-11-16): `plugins/lib/tools/codecs.cpp` prints `aac_ok`
  / `aac` when built with `-DKRAKEN_CODECS_AAC`, which `plugins/Makefile`
  passes - and adds `dab/audio_aaclib.cpp` (self-contained, for that reason)
  - only when that file exists (`$(wildcard ...)`, the same on every
  platform; CMake builds the plugins through this Makefile): plugins move
  as folders, and without `plugins/dab/` the tool builds and prints no aac
  keys. As the tool now holds plugin code the AI assistant may edit,
  `codec_status_message` runs it sandboxed when `plugins/dab` is on the
  assistant's list (`aisandbox::must_sandbox_dir`; without a sandbox the
  check is refused with the usual fix text) - a changed `plugins/lib` marks
  every plugin, so that is covered too while dab exists. `codec_status_message` forwards the aac keys only when the tool
  printed them; the page lists only the codecs the receiver reports (an
  older receiver: no AAC row; an older page ignores the keys). The page's
  row "HE-AAC v2 (libfdk-aac)", used by "DAB+", with apt / MSYS2 / Homebrew
  steps. Docker: the entrypoint exports `KRAKEN_AAC_LIB` when
  `/data/codecs/libfdk-aac.so.2` exists. No Dockerfile / `.dockerignore` /
  `windows/package.sh` change was needed (they take every plugin folder)
- Licences: minimp3 (CC0, compiled into the plugin) and vo-aacenc's AAC code
  book tables in `tools/dab_gen_aac_tables.hpp` (Apache-2.0, generator only,
  but the Windows package carries the prebuilt generator) have
  THIRD_PARTY_NOTICES rows + texts J / K; `scripts/collect-licenses.sh`
  copies minimp3's dedication; libfdk-aac is in section 3 (installed by the
  user)
- Debug playback: `dab-sfn-three` (`tools/scene_sfn3.json`, 3 transmitters,
  expect their talkers at 40 / 150 / 260 deg and the events "Ensemble found:
  Kraken SFN", "Transmitter 10BC-31-15" and Kraken Classic's dynamic label)
  and `dab-single-offset` (`tools/scene_single.json`, one transmitter at 300
  deg, block +24 kHz, CFO +370 Hz, SCO +12 ppm, CIF / midnight wraps; events
  "Ensemble found: Kraken Mux B", "6 services: Kraken One", Kraken One's
  label); WIDE entries (the generator writes the 5-antenna cu8 itself, ~115
  MB, 4-9 s on this PC)
- Tests (job scratch, numbers in the README; i7-11800H): generator scenes
  100 % FIB CRC; `scene_sfn3` 40.1 / 150.3 / 259.8 deg, -4.0 / -8.2 dB,
  +37.5 / +112.2 us, 21 packets each, by code; with 12/7 on k+1: 40.1 /
  149.9 / 259.9 deg, same levels / delays, 12/7 tracked "k/k+1" (also from
  the catalogue's generated `.dat`); `scene_single` +24370 Hz, +11.9 ppm,
  299.9 deg; offsets +-380 kHz block, SCO -35..+40 ppm: 100 % FIBs; SNR 5 dB
  100 % FIBs, 3 dB 75 %, 1 dB none, TII down to 5 dB; logical frames
  bit-identical to the generator's (Layer II UEP, DAB+ EEP A / B: every RS
  word clean at 12 dB). Eight real recordings (DE, AU x2, IT, BE, NL, UK x2)
  100 % FIB CRC (BE 98 % at 6 dB SNR), up to 4 transmitters per ensemble
  with delays. Audio layer on its own: 96 synthetic Layer II streams and 9
  DAB+ configurations every frame / AU ok, PCM corr 1.000000 against ffmpeg
  / a direct fdk decode; real ETI captures of DAB+ (Bayern, Sydney,
  Melbourne) and Layer II (BBC National 15 min: every frame) ensembles;
  through the receiver the generator's DAB+ service played 1749.7 Hz (1750
  Hz) with libfdk-aac 2.0.2. Wrong signals (noise, tone, LTE-like and
  DAB-sized OFDM): nothing. ASan / UBSan clean (scenes, recordings, 2 M
  random FIBs, the audio fuzzer, hostile AAC stand-ins). CPU (plugin's own):
  1.0-1.1 % of real time with one antenna, 1.5-1.7 % with five, 1.3 % with
  Layer II audio; audio 2.2 ms (Layer II) - 5.4 ms (HE-AAC) per second
- Review + end-to-end test (2026-11-16, before the first commit to main): an
  independent review with reproducers (a harness that feeds `on_frame` FIBs
  it encodes itself) found: a reconfiguration announced 6 s ahead (clause
  6.5's lead = the CIF count's lower part's whole period) was applied ~5 s
  early - now an absolute target CIF, applied once, the sub-channel following
  without a break (12 coded cases: 0 lost, 0 wrong); a switch between two
  services of the same codec and bit rate kept the audio layer's state (a
  dynamic label stitched from both) - a programme change now resets it and
  removes the programme's facts; `fig2_`, next-configuration services and
  the row map grew without bound on a hostile FIC (+49.5 MB in 300 s) - now
  capped (64 services, 256 FIG 2 sets, 32 rows / s) and cleared on reset;
  another EId on the block left the old rows / talkers - 3 frames in a row
  now drop the old ensemble; lost CIFs never reached the audio layer; the
  TII gate's power iteration on the noise-subtracted (indefinite) covariance
  gave shares > 1 - now a Jacobi eigen-decomposition, the packet carries the
  PSD part; valid spans / talker packets could straddle samples the host
  dropped - now sent one frame late and only for contiguous frames. The
  debug playback through a TEST build (isolated ports, headless Chromium)
  then found what offline tests can't: the first version of the lost-CIF fix
  took CIF numbers from FIG 0/0 and, in a LOOPED playback (the count steps
  back 200 CIFs per 4.8 s loop), drifted until the modulo-5000 difference
  wrapped - +2400 "lost" and a 0.2 s dropout every 62 s. Now the sample count
  numbers contiguous CIFs and the transmitted count only sets an offset
  (followed after 3 consistent frames): 21 loops = 100.8 s, 0 lost, 0 bad,
  no silent 10 ms block offline; in the playback 36 loops = 171 s per
  service (Layer II and DAB+, both entries): 0 lost, 0 bad, no silent
  stretch in the recorder's audio, ~3 of 50 valid frames withheld per seam
  (the count unconfirmed), the DF packet count rising every loop. Playback results: both catalogue entries 6 / 6 and
  4 / 4 Check rows; bearings over 6-7 loops 40.0 / 150.0 / 259.7 and 299.9
  deg; talkers 2.4-2.8 s after Play; tones 1125.0 / 1750.0 / 599.8 Hz from
  the recorder's audio; FIG 0/22 points at 22 / 35 / 48 km with Δ pos within
  0.2 deg; plugin 3-5 % of a core on an i7-11800H (5 antennas), RSS flat at
  33 MB, 0 dropped. Page fixes from it: re-ticking a VFO's Digital decoder
  goes back to the mode it last ran (`digLastOn`; was Auto detect, which
  never runs a manual-only decoder), the transmitter list drops its Ensemble
  column when the position columns show (was 7 px too wide);
  make_testsignal.py rebuilds a generator when one of its headers is newer
- Not done: a test on the air with the real array (the generator scenes
  through the offline test and the debug playback, and published
  recordings - one antenna each - are all there is; no real transmitter's
  bearing has been checked against its site), a Pi 5 CPU measurement (~3x
  expected), Windows / macOS builds of the plugin and of the codec check
  with the AAC loader (no MinGW / zig on the build machine: only gcc 11 and
  clang 14 syntax checks), reconfiguration on a real or generated RF signal
  (only the harness's coded FIBs / sub-channel) and announcements (no test
  signal has them), DL Plus / MOT / data services
