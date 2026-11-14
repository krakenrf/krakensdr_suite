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
- P25: `ImbeDecoder`, a port of mbelib's IMBE 7200x4400 (ISC; tables in
  `mbe_tables.hpp` with the notice) - Golay/Hamming + PN, parameter decoding,
  mbelib's synthesis with a per-instance RNG (mbelib uses rand()). Verified
  against libmbe on the same frames: correlation 0.9987 with the same random
  sequence (`-DDIG_IMBE_TEST_RAND` switches the RNG for that test). The 9
  frames of an LDU sit at status-free dibit offsets 56,128,220,...,768 and
  are deinterleaved with DSD's iW..iZ (ISC). Muted when the HDU/LDU2 ALGID
  isn't 0x80 or the LC "protected" bit is set
- DMR / D-STAR / NXDN: `MbeLib` dlopens `$KRAKEN_MBELIB` / libmbe.so.1 at first use
  (`KRAKEN_MBELIB=none` disables it) - inside the plugin process
  (no AMBE code shipped or linked); `AmbeStream` keeps a per-stream state in
  3 x 4 KB blocks (mbe_parms is ~1.2 KB). DMR: 3 AMBE+2 frames per voice
  burst (frame 2 straddles the sync/EMB), rW..rZ tables, one slot played at a
  time (option slot, else the first to talk until quiet for 1 s), muted on
  the EMB/LC privacy bit; a burst with no clean frame is not played (false
  syncs). D-STAR: 21 AMBE frames per superframe (frame 0 is before the data
  sync), dW/dX tables, stops at the end pattern
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
- Codec availability: the sidebar box (above), and each voice plugin sets a "Voice codec" fact (e.g.
  "AMBE+2: libmbe.so.1 (mbelib 1.3.0)" / "not installed"), and its voice
  state says "... codec not installed" while someone listens
- Docker: the entrypoint exports `KRAKEN_MBELIB` / `KRAKEN_TETRA_CODEC_DIR`
  when `/data/codecs` holds libmbe.so.1 / cdecoder + sdecoder (inherited by
  the plugin processes)
- Tested: offline (regression incl. voice), through the real Opus audio
  stream with a fake heimdall replaying recordings of all four, and live:
  DMR Tier III traffic-channel speech here; P25 calls here are AES-256 and
  are correctly muted (all before the move into plugins; the regression
  re-verified it after)

