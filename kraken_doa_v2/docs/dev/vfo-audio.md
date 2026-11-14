# VFO audio

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**🔊 VFO audio - any number of VFOs heard at once (`src/signal_processing/audio_mixer.cpp`, the speaker toggle on each VFO card):**
- The user's request (2026-10-17): the "Audio Src" radio button became a
  speaker toggle next to each card's D0 / D1 ... title - green with sound
  waves = heard, red with a cross = muted - and several VFOs can be heard at
  once. `DecimatorInstance::audio_out` (atomic, per VFO) replaced
  DecimatorManager's single `fm_decimator_id`: `setAudioVfo` /
  `setAudioVfos` / `audioVfoIds` / `primaryAudioVfo` (the first VFO heard in
  LIST order, -1 = none - where one VFO stands for "the one you listen to":
  the beamforming readout, the recorder's band VFO -1, independent mode's
  CH following the VFO's tuner). New VFOs start muted; the first VFO is
  heard on a fresh start; removing the LAST VFO heard makes the first one
  left heard (as the single source moved before) - not when every VFO had
  been muted
- Server-side mix, ONE audio stream: the pipeline queues an FM work item
  per VFO heard per block (`DecimatedFMWorkItem::vfo_id`, queue cap 20 per
  VFO); the FM thread feeds each into its OWN `AudioMixer::Channel` - an
  `FMDemodulatorRobust` (demodulator, filters, levels and its 48 kHz ring),
  rate and demod taken from the VFO block by block (so the old
  `fm_demod.setDemodulatorMode / setInputSampleRate` calls in the control
  handler are gone), its squelch (FFT / beamformed FFT / Digital, the old
  FM-source code per VFO) and Digital demod (`pull_voice` of its decoder);
  the 40 ms audio timer (`build_audio_message`) reads every channel's ring
  (each with its own initial fill, exactly as the single source's) and SUMS
  them, soft-limited above 0.95 (tanh) - one VFO open at a time (squelch)
  plays at its own level - into one Opus encoder (32 kbit/s VBR, as
  before): the page's player is unchanged. No VFO heard = encoded silence
  (the stream keeps flowing). A channel not fed for `IDLE_MS` 3 s leaves
  the mix (it comes back with the next block); a VFO muted is dropped at
  once (`retain()`); FM:0 drops them all. `FMDemodulatorRobust` makes its
  Opus encoder lazily now (only `get_opus_packet` uses it - the live
  channels and the recorder's demodulators never encode)
- Commands: `AUDIO_VFO:<id>:0|1` (the toggle; `is_query_command`, every
  page gets `decimator_info`), `SET_FM_DECIMATOR:<id>` kept = hear only
  that VFO (older pages / scripts); `decimator_info[]` carries `audio` and
  still `is_fm_source` (= audio, for pages from before). Persisted in the
  DECIMATORS snapshot's head: list positions "0,2", "-1" = none (was one
  position; an older kraken_doa reads the first, "-1" as the first VFO; an
  old "1" loads as only the second VFO). `ControlHandler::set_audio_vfos` /
  `listen_only` (thread-safe, no snapshot save: the continuous scanner
  listens only to the VFO it locks - `audio_before_` is put back at stop)
- UI (kraken_doa.html, `decSpkRender` / `setVfoAudio` / `toggleVfoAudio`,
  `decAudioOn(d)` = `d.audio`, older receivers `is_fm_source`): an inline
  SVG button `#dec-spk-<id>` (`.dec-spk.on` #4caf50 / `.off` #ef5350,
  aria-pressed, a tooltip), shown at once and confirmed by the receiver's
  decimator_info; refreshed in place (not part of updateDecimatorList's
  structure check). The top bar's Audio off dims them (`.mute-all`, said in
  the tooltip); clicking a muted speaker while it is off switches the audio
  on too. The decoder tab's 🔊 Listen adds the VFO (Digital demod); "🔊
  Listening" goes back to the demod it had before Digital, heard or muted
  as it was then, the bandwidth kept (`digDemodBefore` in the globals
  block, set by `setDecimatorDemodMode` on entering Digital, localStorage
  `kraken_dig_demod_before`; nothing remembered - Digital set by another
  page - = WBFM on a VFO >= 150 kHz, else muted as before:
  `digListenBack`). It only muted it until 2026-10-22: the RDS plugin's SCA
  audio had no way back to the station's WBFM but the card's Demod list,
  which also reset 400 kHz to 240 kHz (the 92 kHz SCA gone). That list
  (`decDemodPicked`) now keeps the bandwidth too when it goes back to the
  remembered demod; any other demod gets its usual bandwidth as before
  (saved frequencies / the assistant call `setDecimatorDemodMode` as
  before). Test: job scratch 8e472af8 t_listen.py (headless Edge, real
  clicks, the rds playback on a test build: 35 checks). The spectrum title lists the VFOs heard ("D0
  NBFM, D2 AM"). The recorder's band VFO default reads "The VFO you listen
  to (the first 🔊)"
- LATENCY CONTROL (2026-10-23, the user's report: "when CPU usage is high
  the audio starts to lag ... and never recovers" - heard late against the
  spectrum for good). Three buffers only ever grew: (1) each channel's
  48 kHz ring (`FMDemodulatorRobust`, 1.36 s) - a stalled FM thread made it
  underrun, the backlog then came in a burst and stayed; full, `write`
  drops the NEWEST samples, so it stayed 1.36 s late; (2) the audio timer
  on Linux: uSockets reads the timerfd's expiration count and runs the
  callback ONCE, so every 40 ms tick a busy loop missed was a packet never
  sent (the ring grew by it); (3) the page's worklet queue (kept up to
  10 s; packets arrive in bursts whenever the page's main thread - which
  decodes the Opus - is busy, or the network hiccups). Fixes: the timer
  ticks at 20 ms and sends the packets due by the steady clock on EVERY
  platform (was Windows only; <= 3 per tick, a clock > 200 ms late
  restarts); `get_audio_buffer`: a ring that held more than
  `AUDIO_TRIM_ABOVE` 0.2 s before every read of a 2 s window
  (`AUDIO_TRIM_WINDOW_READS` 50) drops down to `INITIAL_FILL_TARGET` 0.1 s,
  crossfaded over 5 ms (`AudioRingBuffer::skip` / `readable`, consumer
  side; stdout "Audio: N ms of late audio dropped"), and an underrun plays
  what is left faded out and buffers 0.1 s again (it used to play on at the
  empty edge - a gap at every dip); the worklet (kraken_doa.html
  `setupWorklet`): a Float32Array ring (5.5 s; `splice(0,128)` of an Array
  moved the whole queue every quantum) with the same rule - the least it
  held over 3 s above floor + 0.1 s is dropped to the floor (crossfaded over
  one quantum), > 2 s ahead at once; the floor (150 ms) adapts to STALLS in
  the packets' arrival (an underrun after an arrival gap > 120 ms and < 5
  s: +50 ms, a second within 60 s straight to that gap, max 1 s; -25 ms per
  calm minute) - not to a sound card a little faster than the receiver's
  clock. Measured (job scratch b2542a52: `t_ring.cpp` = the real
  fm_demodulator / ring_buffer in virtual time, the old and the new
  sources; `gen_worklet_test.py` = both pages' worklet code in headless
  Chrome under the same scenarios + the new one in a real
  OfflineAudioContext): server ring old / new, latency at the end - FM
  thread stalled 0.8 s 796 / 100 ms, timer loop busy 0.5 s 599 / 100 ms,
  receiver clock +0.2 % 357 ms and growing / <= 207 ms, -0.2 % 303 gap
  packets / 2 rebuffers, steady and 60 ms jitter identical (117 / 114 ms,
  no trims); worklet old / new - page busy 1.5 s 1517 / 237 ms, tab frozen
  6 s 6037 / 117 ms, sound card 0.1 % slow 479 ms growing / <= 291 ms,
  periodic 300 / 700 ms network stalls the old one's latency after 3 gaps
  (old: 1 gap), steady identical (197 ms)
- Tests (job scratch 46eef211, test build on 5708x, fake heimdall with FM
  tones 1 kHz at -300 kHz and 2.5 kHz at +500 kHz - `--f2 / --tone2`):
  t_ma.py decodes the real Opus stream (libopus via ctypes) and measures
  both tones: D0 only / both (level = sum, not halved) / D1 only / none
  (silence, packets flowing) / D1 squelch closed / SET_FM_DECIMATOR / FM:0
  (no packets) + FM:1 / restart restores "0,1" / an old "1|" snapshot / the
  last VFO heard removed / unknown VFO refused - 23 / 23; t_ma2.py DoA off
  (fm_only, one channel) and beamforming on (incl. a 10 kHz VFO's bursty
  FD-DAS output) - 5 / 5, no ring under- / overruns; t_ma_ui.py (headless
  Chromium, real clicks): speakers green / red with the right icons next
  to the title, no radio buttons, title, another client's change followed,
  audio-off dimming + click-to-hear, Listen, an older receiver's
  is_fm_source - 22 / 22

