# Digital voice

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Digital voice (`plugins/lib/dig_vocoder.cpp`, Demod "Digital"):**
- `DemodulatorMode::DIGITAL`: the FM thread still runs the FM demodulator
  (as NBFM - `setDemodulatorMode` maps it) only because its output length
  clocks the audio stream; for every DIGITAL VFO heard (🔊) the samples are then
  replaced by `DigitalDecoder::pull_voice()` (silence when there is none) and
  squelch is skipped. `pull_voice` also marks the voice "wanted" for 0.5 s -
  the lead plugin gets VOICE_WANTED and runs its vocoder only then.
  `SET_DECIMATOR_DEMOD:id:DIGITAL` switches the VFO's decoder to AUTO if off
- Voice FIFO (digital_decoder.cpp): 8 kHz plugin audio (AUDIO messages) ->
  slow AGC (tanh limited) -> 6x interpolation -> 48 kHz FIFO; playback starts
  at 250 ms buffered (frames arrive in bursts, a P25 LDU = 180 ms) or once
  the input stops for 300 ms (a call's tail), re-prefills after an underrun
- P25 Phase 1: `ImbeDecoder`, a port of mbelib's IMBE 7200x4400 (ISC; tables in
  `mbe_tables.hpp` with the notice) - Golay/Hamming + PN, parameter decoding,
  mbelib's synthesis with a per-instance RNG (mbelib uses rand()). Verified
  against libmbe on the same frames: correlation 0.9987 with the same random
  sequence (`-DDIG_IMBE_TEST_RAND` switches the RNG for that test). The 9
  frames of an LDU sit at status-free dibit offsets 56,128,220,...,768 and
  are deinterleaved with DSD's iW..iZ (ISC). Played only once the call is
  known to be clear (2026-11-19): ALGID 0x80 (HDU / LDU2) or a voice-user
  LC without "protected"; muted when the ALGID isn't 0x80 or the bit is
  set. A call joined at an LDU1 with another LC holds up to 2 LDUs until
  the next LDU2, then plays them in order (clear) or drops them - before,
  real encrypted calls joined late played 180 ms of noise. Held and
  released, two LDUs reach the voice FIFO together and start playback at
  the same moment as two LDUs 180 ms apart (prefill 250 ms), so a clear
  call joined late loses nothing (plugins/p25/README.md)
- Output limiter (`VoiceLimiter`, 2026-11-19): `ImbeDecoder` and
  `AmbeStream` (so P25 both phases, DMR, D-STAR, NXDN, YSF) pass every 20
  ms frame through a per-stream peak limiter instead of clamping at +-1.
  The codecs run at mbelib's fixed gain (x7 into 16 bits; mbelib itself
  hard-clips at +-32760, no gain control), and a real loud P25 talker's
  error-free frames reached +18 dB over full scale (5 % of samples
  clipped). While idle a frame within +-1 passes bit for bit; a frame over
  full scale sets the gain so that it peaks at -1 dBFS (ramped over up to 4
  ms where the leading samples allow, else at the frame start), recovering
  at 15 dB/s ramped over each frame - no clipping, no delay. Why in the
  plugin: the AUDIO message is float32, but Voice to Text
  (`voice2text.cpp`), the audio recorder (`recorder_audio.cpp`) and the
  offline host's wav clamp the raw plugin audio at +-1, and the player's
  AGC (Voice FIFO above: gain 0.5-30, tanh) can take off only 6 dB - the plugin must
  deliver it within +-1. Checked: every catalogue scene of the five voice
  plugins (their placeholder / random voice bits clipped 0.06-4.8 % of
  samples before) and every P25 recording - all frames bit-identical except
  stretches that start at a frame that clipped before; fuzz test (600,000
  frames) never beyond +-1. `MbeLib::decode` / `decode49` return the scaled
  but unlimited output (only `AmbeStream` calls them)
- DMR / D-STAR / NXDN: `MbeLib` dlopens `$KRAKEN_MBELIB` / libmbe.so.1 at first use
  (`KRAKEN_MBELIB=none` disables it) - inside the plugin process
  (no AMBE code shipped or linked); `AmbeStream` keeps a per-stream state in
  3 x 4 KB blocks (mbe_parms is ~1.2 KB). DMR: 3 AMBE+2 frames per voice
  burst (frame 2 straddles the sync/EMB), rW..rZ tables, one slot played at a
  time (option slot, else the first to talk until quiet for 1 s), muted on
  the EMB/LC privacy bit; a burst with no clean frame is not played (false
  syncs). D-STAR: 21 AMBE frames per superframe (frame 0 is before the data
  sync), dW/dX tables, stops at the end pattern
- P25 Phase 2 (2026-11-18, plugins/p25/p25p2.cpp; plugins/p25/README.md):
  AMBE+2 3600x2450 through `AmbeStream`, 4 frames per 4V burst and 2 per
  2V, in DMR's bit order (rW..rZ - 93.4 % of real frames with no c0 error
  in it), after descrambling (voice bursts are scrambled: 0.2 % of 18,398
  real frames have a clean Golay c0 as received, 95.7 % descrambled; the
  frames' c0 decides per burst). One `AmbeStream` per logical channel, the
  4V 4V 4V 4V 2V cycle placed by MAC_PTT's offset field; ESS (ALGID / KID /
  MI) by RS(44,16,29), the 4V segments collected over the cycle, missing
  ones as erasures. Option `lch` (Timeslot) as DMR's `slot`: Both = the
  first to talk until quiet for 1 s, but a muted call doesn't hold the
  audio against a clear one; the hold runs on sample time, not the wall
  clock (offline it followed the wall clock and stuck to one timeslot for
  a whole file - fixed 2026-11-18). Muted when encrypted (ALGID not 0x80,
  or the "protected" service option) AND until a call is known to be clear
  (a call joined after its MAC_PTT: its first bursts on real encrypted
  calls came before the ESS and played as noise); those bursts are dropped,
  not held as Phase 1's LDUs are. The audio's talker
  channel (`voice_channel` -> `Host::audio_channel`, Voice to Text's
  naming): Phase 1 0 (set before each IMBE block - before Phase 2 existed
  it was never sent, = unknown, which named the same talker), Phase 2
  outbound LCH + 1, inbound -1 (unknown: inbound radios are packets). With
  Timeslot LCH 0 / LCH 1 an inbound call whose LCH isn't known yet is
  neither shown nor played. Real recording: 1172 of
  1172 frames clean, 20 s of audio with no vocoder errors; one unit there
  is loud at the source (coded gain 12-18 dB above the other timeslot's
  talker) and clipped 5 % of its samples before the output limiter (above;
  now rms -18.4 dBFS, nothing clipped)
- YSF (2026-11): VW's 144-bit full-rate frames (88 + 56 bits at 7200 bit/s
  = the IMBE frame) through `ImbeDecoder`, de-interleaved in the YSF order
  (`vw_plain_index` in plugins/ysf/ysf.cpp - not P25's iW..iZ); V/D mode 1's
  72-bit AMBE+2 frames through `AmbeStream` with rW..rZ (as DMR); DN's 49
  bits (after the plugin's own majority / de-whitening) through mbelib's
  `mbe_processAmbe2450Dataf` (`MbeLib::decode49` / `AmbeStream::decode49`,
  dlsym'd as optional - without it the voice status says DN needs it; the
  error count passed is the triplets that disagreed). Yaesu's standard
  doesn't give the bit order inside any voice frame: all three placements
  checked against dsdcc's recovered frames (2026-11-16; the VW order was
  P25's before - wrong) and, 2026-11-17, on off-air recordings (VW: every
  Golay word of 422 real frames valid as received; DN: the pitch parameter
  runs in smooth tracks in this order only - plugin README); the audio
  of both recordings is speech (2026-11-17, mbelib 1.3.0: Voice to Text
  writes down the callsign spelled out, as dsdcc + mbelib's audio); V/D
  mode 1 only with the generator's random frames. DN silence frames (b0
  124 / 125) are muted (`voice_vd2`): mbelib 1.3.0 decodes their other
  bits as gain / spectrum, and a call that opened with five of them went to
  full scale 60-140 ms in, the speech after inheriting the gain (dsdcc +
  mbelib the same). Leading ones (nothing else since the codec's reset)
  also reset the codec, later ones (pauses) keep its state - the speech
  after a pause as before. First 0.5 s peak 32767 -> 5919, whisper one of
  two phrases (0.51) -> both (0.71); plugin README. The 49-bit DN path
  only - the shared 72-bit path (DMR / NXDN / V/D 1) is unchanged
- TETRA: `TetraCodec` runs the ETSI programs as `cdecoder /dev/stdin
  /dev/stdout | sdecoder ...` (posix_spawn, non-blocking pipes, 690-word
  test-frame blocks of descrambled type-4 soft bits). Full-slot traffic
  (AACH usage marker >= 4, training sequence 1) of one timeslot; after 400 ms
  without traffic, erasure blocks flush the pipes' stdio buffers. Muted for
  60 s after any encrypted MAC-RESOURCE. ~0.5 s extra latency (pipe buffers)
  Windows (no /dev/stdin there): each program gets two NAMED PIPES as its
  file names (`\\.\pipe\krakensdr-tetra-<pid>-<n>-<64 random bits>`, one
  instance, local only, the server end the plugin's; its client checked by
  process ID - GetNamedPipeClientProcessId - before any data), started
  through kproc (NUL stdio, no console); three plugin threads (writer ->
  cdecoder, relay cdecoder -> sdecoder, reader) in overlapped I/O ended by a
  stop event, so data moves through both programs between slots as on
  Linux. A program that exits / breaks a pipe: stopped, restarted no sooner
  than 2 s later (not every slot). The ETSI code needs no change: fopen
  "rb" / "wb" + sequential fread / fwrite open a pipe like a file (binary).
  Found as `$KRAKEN_TETRA_CODEC_DIR\{cdecoder,sdecoder}.exe`, else
  `tetra-{cdecoder,sdecoder}.exe` in the working directory (kraken_doa_v2,
  where libmbe.dll goes), else on the PATH. Tested 2026-10-20 with
  stand-in programs that use the same file I/O: byte-exact at real-time
  pace (~0.47 s to the first audio), only whole frames dropped when flooded
  (the 64-block cap, as Linux), a codec that dies is restarted, one that
  hangs doesn't block stop(); and with the real ETSI codec built by the
  WINDOWS.md steps (MinGW UCRT64, only the system UCRT DLLs; MSYS2's patch
  needs `--binary` - CRLF sources): 200 random slots and a TTS sentence
  encoded with scoder + ccoder decode through TetraCodec bit-identical to
  cdecoder + sdecoder on plain files, and to the Pi's build / pipes
  (fixed-point code: the same samples on both OSes); the sentence's
  correlation with the spoken input 0.948 (codec delay 40 samples). ccoder
  writes a 1 as 0x0081 (129), TetraCodec sends -127 - cdecoder decodes both
  the same
- DAB / DAB+ (the dab plugin, not `plugins/lib`; *plugin-dab*): MPEG Layer
  II through the vendored minimp3 (CC0, built in); DAB+ HE-AAC v2 through
  Fraunhofer FDK AAC, which `plugins/dab/audio_aaclib.cpp` dlopens at first
  use the way `MbeLib` does: `$KRAKEN_AAC_LIB` (`none` / `off` disables
  it), else `libfdk-aac.so.2`, `/usr/local/lib/libfdk-aac.so.2`,
  `libfdk-aac.so.1` (Windows `libfdk-aac-2.dll` ..., macOS
  `libfdk-aac.2.dylib` + the Homebrew paths). Decoder module versions
  below 2.x and a library missing a function are refused. Never shipped or
  linked (THIRD_PARTY_NOTICES section 3); without it DAB+ still syncs, its
  RS / AU CRCs and the dynamic label run, only PCM is missing
- Inmarsat Classic Aero C channel (2026-11-22, `plugins/inmarsat_aero`;
  *plugin-inmarsat-aero*): AMBE 4.8 kbit/s (96-bit frames, 25 per 500 ms C
  channel frame) through `dig::AeroAmbeLib` / `AeroAmbeStream`
  (`plugins/lib/aero_ambe.*`), which dlopens an mbelib that has
  `mbe_processAmbe4800x3600Framef` - libaeroambe's `mbelib-master` (mbelib
  1.3 + the Aero codec, CMake, no Qt): `$KRAKEN_AERO_AMBE` (`none` / `off`
  disables it), then `$KRAKEN_MBELIB`, then the names `MbeLib` tries - the
  first library with the function (the standard mbelib 1.3 lacks it and is
  skipped; libaeroambe's build exports everything `MbeLib` needs too, so
  one file can serve both - checked with `nm -D`). The frame's 96-entry
  deinterleave table is libaeroambe's (MIT, notice in the file and in
  THIRD_PARTY_NOTICES text M) - a bit order like DSD's tables in
  `mbe_tables.hpp`; no codec code or codec table is in the repo. "Voice
  codec" fact: "AMBE (Aero 4800): <file> (mbelib 1.3.0, with the 4800x3600
  codec)" / "not installed (libmbe.so.1 lacks the Aero AMBE 4800x3600
  functions - see README: Digital voice codecs)" / "disabled
  (KRAKEN_AERO_AMBE=none)"; output through `VoiceLimiter`. The Setup box's
  codec list (`lib/tools/codecs.cpp`) does not show it. STATE OF TESTING:
  the path runs end to end in a playback (generated C channel, random
  voice bits: 502 of 551 Opus packets non-silent with the codec, all
  silent without) - but NO real C channel recording could be found, so
  that speech comes out of a real call is unverified
- Codec availability: ⚙ Setup Settings → Decoders (above), and each voice plugin sets a "Voice codec" fact (e.g.
  "AMBE+2: libmbe.so.1 (mbelib 1.3.0)" / "not installed"), and its voice
  state says "... codec not installed" while someone listens. The page
  shows a missing codec in red (#ef5350, the page's alarm red; 2026-11-17,
  so it is noticed): the fact when its value, at its start or after a
  "NAME: " prefix, has "not installed" / "disabled", or has "lacks the" /
  ends in "not used)" / "(no DN voice)" (`digCodecFactBad`: mbelib's and
  TETRA's "not installed (see README...)", "disabled (KRAKEN_MBELIB=none)",
  "... lacks the AMBE functions", the AAC library's refusals; ysf names two
  codecs, "VW: IMBE (built in); DN, V/D mode 1: AMBE+2: not installed ..."
  - since the merge with main 2026-11-17 the prefix may be any "...: ", so
  its missing AMBE part is red too, and so is its mbelib without
  mbe_processAmbe2450Dataf, "..., without ... (no DN voice)"); the voice
  state when it has "codec not installed / disabled / not usable" or
  "mbelib lacks" (ysf's DN) (`digVoiceStateBad`, only while listening);
  the sidebar box's missing rows (✗ and status line). Only the "Voice
  codec" fact is looked at - another fact with these words stays as it
  was - and a path (TETRA's "ACELP: <path> + <path>", a Windows "C:\...")
  can't match: the words must follow the start or a colon and a space. A
  new voice plugin keeps these wordings. The box's
  data is `plugins/lib/build/codecs` (`lib/tools/codecs.cpp`): imbe / ambe
  / acelp always, and since 2026-11-16 `aac_ok` / `aac` ("HE-AAC v2
  (libfdk-aac)", DAB+) when the tool was built with `plugins/dab/` there -
  plugins/Makefile compiles `dab/audio_aaclib.cpp` (self-contained) into it
  with `-DKRAKEN_CODECS_AAC` only if that file exists - so a `plugins/dab`
  the AI assistant changed makes kraken_doa run the tool sandboxed
  (`aisandbox::wrap`; refused without a sandbox, *AI assistant*), as the
  plugin itself. kraken_doa forwards the aac keys only when the tool printed
  them, and the page lists only the
  codecs the receiver reports (an older receiver: no AAC row; an older page
  ignores the keys)
- Docker: the entrypoint exports `KRAKEN_MBELIB` / `KRAKEN_TETRA_CODEC_DIR`
  / `KRAKEN_AAC_LIB` when `/data/codecs` holds libmbe.so.1 / cdecoder +
  sdecoder / libfdk-aac.so.2 (inherited by the plugin processes)
- Tested: offline (regression incl. voice), through the real Opus audio
  stream with a fake heimdall replaying recordings of all four, and live:
  DMR Tier III traffic-channel speech here; P25 calls here are AES-256 and
  are correctly muted (all before the move into plugins; the regression
  re-verified it after)

