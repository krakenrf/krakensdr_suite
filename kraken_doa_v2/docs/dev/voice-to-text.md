# Voice to Text

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**🗣 Voice to Text (`src/voice2text.cpp`, `include/voice2text*.hpp`, `transcribe/`, sidebar "🗣 Voice to Text", the 🗣 Voice tab):**
- What is said on a VFO, written down by a LOCAL, free model: whisper.cpp
  (MIT, pinned v1.9.4 = `WHISPER_COMMIT` in `transcribe/setup.sh`) with
  OpenAI's Whisper models (ggml-*.bin from huggingface.co/ggerganov/whisper.cpp)
  + Silero VAD (ggml-silero-v6.2.0.bin from ggml-org/whisper-vad). Opt-in:
  `make whisper` = `transcribe/setup.sh` (clone + CMake build -j3 max ~5-10
  min + base.en + VAD; `--models-only M..`, `--build-only`, `--list`,
  `--update` = what `make all`'s `whisper-update` runs once it is set up:
  incremental, quiet, re-pins whisper.cpp); `WHISPER=1 ./install.sh`
- Per VFO: `DecimatorInstance::transcribe` (VFO card "🗣 Transcribe",
  `VOICE2TEXT_VFO:id:0|1` -> `DecimatorManager::setTranscribe`, which also
  sets the decoder's voice tap; DECIMATORS snapshot field 11, optional -
  older files load with it off). `anyTranscribeActive()` makes the pipeline
  decimate (in fm_only mode the VFO counts as `single_wanted`)
- Source = the VFO's own stream, not what the 🔊 speakers play: with its DIGITAL
  DECODER ON and the decoder decoding voice (`DigitalDecoder::takes_voice()`:
  the lead plugin's `kp::Info::voice`; Auto detect still searching: any of
  its plugins; set by the engine's housekeeping) the decoder's voice; a
  decoder of NO voice (analog_id, pagers, adsb ...) leaves the ANALOG path below
  to the VFO, as without one - and names the transcript after the
  decoder's talker (`DigitalDecoder::lead_talker`: the lead plugin's open
  talker that started first, e.g. "CTCSS 88.5" / "88.47 Hz" as from / to;
  `feed_analog(.., tid, tlabel)`; another talker after it while the gate
  stayed open splits the transcript where the first one was last named, as
  `Worker::digital` does) - `dig::set_voice_tap_handler` (one
  global handler) gets the lead plugin's W_AUDIO 8 kHz + plugin id + its
  talker + RF, on the decoder's worker thread. The talker: the open one on
  the channel the plugin says its audio is from (`kp::Host::audio_channel`
  -> wire AUDIO_CHANNEL 34 -> `Runner::audio_channel`, reset on a restart;
  the dmr plugin sends the timeslot it plays via `RxContext::voice_channel`),
  else the open talker that STARTED first (decoders play the first to
  talk). It was the most recent one across channels: with both DMR slots
  busy, slot 1's speech was named after slot 2's radio and split when that
  call ended (review 2026-10-13). `voice_tap()` also counts as VOICE_WANTED, so the
  vocoder runs while nobody listens (a pager / non-voice decoder gives
  nothing). Otherwise ANALOG: data_receiver feeds `v2t::feed_analog` after
  the digital push (same stream: beamformer output when it ran, else the
  listened-to channel) with the VFO's demod and a gate = its squelch_open
  when the squelch is on (FFT or Digital), else
  `check_squelch_in_range(.., AUTO_SQUELCH_DB 10 dB)` - the same FFT test as
  the squelch, so a squelch-off VFO still only records while a signal is
  there
- ONE worker thread ("voice2text") owns the taps, the job queue and the
  helper process; inputs are copied into a queue (q_mu; <= 3 s per VFO
  waiting, else dropped), status / history are read under st_mu. Per tap:
  analog demod at the VFO rate -> msresamp to 16 kHz -> Butterworth HP (260
  Hz NBFM / AM - CTCSS out -, 100 Hz WBFM) + LP (3.8 / 7 kHz). FM = atan2
  discriminator / deviation (5 kHz NBFM, 75 kHz WBFM, 75 us de-emphasis for
  WBFM only - as the live audio); AM = envelope - 30 Hz DC, power AGC 0.5 s.
  Digital = 8 kHz x2 (firinterp)
- Cutting (`Worker::analog / digital / finish`): analog starts on the gate
  with 0.3 s pre-roll, ends after the gate is closed `HANG_S` 0.8 s (0.25 s
  tail kept), or `ANALOG_STALL_MS` 2 s after its input stopped (heimdall
  gone: it used to stay open and glue the audio from after the gap on);
  digital ends `DIGITAL_GAP_MS` 1.5 s after the last voice, or at a talker
  change. Unnamed audio ("" / "?") after a named talker only marks where it
  began (`Tap::anon_from`): the next name decides - the same radio = one
  transmission (P25 closes a talker after a 0.75 s pause), another radio =
  `split()` there (the next radio's first frames come before the plugin
  names it); a transmission that starts unnamed adopts the first name.
  RF move > 500 Hz / demod / rate change ends the transmission. < 0.4 s
  dropped. Long audio -> pieces (`part` 1, 2..; `maybe_cut` / `cut` after
  every input block): two-way = ONE transcript up to `TWOWAY_MAX_S` 25 s
  (its text comes at its end anyway; whisper does better with the whole -
  cutting at 18 s split 20 s transmissions for nothing), then cut at its
  latest pause after `TWOWAY_MIN_S` 11 s; CONTINUOUS (a WBFM VFO, or
  `tx_samples` - the transmission so far - > `CONTINUOUS_S` 30 s: AM
  broadcast, a weather channel) from `CONT_SOFT_S` 8 s on at the latest
  pause after `CONT_MIN_S` 5 s, so the text follows ~10-20 s behind. Pause =
  `PAUSE_FRAMES` 12 x 20 ms frames (`Tap::fe`, energies kept as `cur` grows)
  under max(0.1 median, 2 x 10th percentile) <= 0.5 median - a tenth of the
  median alone found NO pause in hissy radio audio (an Apollo recording's
  gaps sit at a third of the median), cut in its middle. No pause by the
  maximum (15 / 25 s): at the quietest 100 ms of the last 3 s, the next
  piece repeating `OVERLAP_S` 0.3 s. DoA: while recording, each NEW published MUSIC frame
  (`getResultStampMs`, not stale, coherent, DoA on) adds its compass-sense
  bearing (360 - peak, as the DoA logger) weighted by confidence -> circular
  mean = `doa`; `brg` = doa + station heading when valid. With the Digital
  squelch the VFO's DoA is published from confirmed frames only, so a
  digital transmission's bearing is the talker's
- LOBE per transcript (`sample_doa`, the same frames + weights as `doa`):
  each frame's pseudospectrum (`getPseudospectrum`, |v|, min..max -> 0..1,
  array angle = index x res, linear between bins) summed into
  `LOBE_BINS` 720 x 0.5 deg = the map's live-lobe format (rdf_mapper.cpp
  `display_lobe`: compass tau = heading - array angle) - TWICE: `lobe_n`
  turned by the heading of the frame's moment (frames with a heading - a
  vehicle turning during a transmission stays right) and `lobe_a` with
  heading 0 (relative to the array, all frames); `finish` keeps lobe_n when
  >= half the weight had a heading (`lf` "n"), else lobe_a (`lf` "a"), min..max
  -> u8 -> base64 (960 chars), + the station's mean position (`slat` /
  `slon`, longitudes around the first - antimeridian). Parts of a long
  transmission each get their own (`Tap::reset_doa`). In transcripts.jsonl
  (`transcript_json(t, true)`: lobe, lf, slat, slon; a bad slat keeps the
  lobe, a malformed lobe - not 960 base64 chars - is dropped), NOT in the
  pushed lists (`transcript_json(t, false)`: only `"lb":"n|a"`) - 300
  history items x 1 KB to every page on every connect.
  `VOICE2TEXT_LOBE:<id>` (digits only, `is_query_command`) -> broadcast
  `v2t::lobe_message(id)` = `{"voice2text":{"lobe":{id, t, vfo, mhz, dur,
  lobe, lf, doa, brg, slat, slon}}}` or `{id, error}`; the page that asked
  (`v2tLobeWant`) shows it, the others ignore it
- Queue: <= `MAX_QUEUED_JOBS` 40 / `MAX_QUEUED_S` 600 s (then the oldest
  go, `dropped`); `usable_` (helper + model files there) else input is
  ignored and the queue cleared (no "dropped" spam while not set up)
- Helper `transcribe/build/kraken_whisper` (transcribe/kraken_whisper.cpp,
  CMake: whisper.cpp via add_subdirectory, static) started with
  `PluginProcess::start(argv)` (new overload; same pipes / stderr tail as
  plugins): `--serve --model M --threads N [--vad V]`, nice 10 (it renices
  itself). Wire `include/voice2text_wire.hpp` (plugin framing): JOB = id +
  int16 16 kHz, OPTION key=value (language, prompt, threads, vad,
  vad_threshold, translate, beam), READY (model, type, multilingual, vad,
  version, system), RESULT (id, conf, no_speech, proc_ms, speech_ms, lang,
  text, dropped reason), ERROR. ONE job at a time (the next is sent after
  the RESULT); options re-sent when they change; a model change restarts
  it. Crash -> the in-flight job is retried once, restart after 2..30 s; a
  job running > max(90 s, 15x its length) kills it. No VFO transcribing for
  `HELPER_IDLE_STOP_MS` 2 min -> stopped (base.en ~280 MB RAM)
- In the helper per job: Silero VAD (min speech 200 ms, min silence 300 ms,
  pad 200 ms) -> only the speech parts, joined by 150 ms of silence (< 200
  ms of speech = "no speech", whisper never runs: 30-40 ms for 5 s of noise)
  -> whisper_full (greedy, no_context, no_timestamps, suppress_nst, language
  "en" for .en models) -> segments with no_speech > 0.6 AND avg logprob < -1
  skipped -> [..] (..) *..* and music notes stripped, a sentence / word
  repeated 4+ times kept once -> dropped: empty, subtitle-credit phrases
  ("thanks for watching"...), lone fillers ("thank you", "you", "bye"...)
  when no_speech > 0.35 or conf < 0.5. conf = mean token p. Greedy decoding
  sometimes ENDS after the first sentence ("Roger the register." of 11 s of
  speech; the clip offline gave the whole text): a result with < 0.6 words
  per second of speech (>= 4 s) is decoded again as two halves of the
  speech, split at the VAD gap nearest the middle (`joins`), the longer
  text wins. Timestamps (whisper.cpp seeks on from the last one) did NOT
  help - same text, and always on they broke a clip that decoded fine.
  Our own VAD
  (not whisper_full's vad flag): its segment list goes stale when a clip has
  NO speech, so speech_ms couldn't be read
- PROMPT ("Words to expect", `VOICE2TEXT_PROMPT:`, <= 300): whisper reads it
  as the text said before, and a bare list ("Houston, Roger, over") made
  greedy decoding END after one word ("the", conf 0.99) on 2 of 4 test
  transmissions - with or without carry_initial_prompt / timestamps /
  no_context. `prompt_text()` turns a list into a finished sentence
  ("Glossary: Houston, Roger, over.") - all 4 decoded fully; and a prompted
  result with < 0.4 words/s over >= 2.5 s of speech is decoded again
  without the prompt (the longer text wins). Beam search was no cure (one
  gave "Later.")
- Outputs (`on_result`): history (last `MAX_HISTORY` 1000 in memory,
  appended to `transcripts.jsonl` - cwd, gitignored; rewritten from the
  history when it passes 3000 lines, `rewrite_history_locked`; a line that
  doesn't parse is skipped at startup - `stod` threw and the supervisor
  restarted into the same crash), `declog::record_voice`, the clip (below),
  `fresh` for the push - the clip list, file and history in ONE
  st_mu section that drops the result when `clear_gen` moved since its id
  was taken (a Clear meanwhile brought it back, clip gone). Transcript id =
  max(last + 1, t0 ms) (unique, sortable, survives restarts). JSON {id, t,
  dur, vfo, mhz, src, tid, tl, doa, brg, lb (pushes) / lobe, lf, slat, slon
  (the file), part, conf, lang, ms, clip, text}.
  The status `error` = `last_error`, kept until the helper loads (READY) or
  a job goes through (a crash's message used to be overwritten by
  "starting" within ms)
- Pushes: `{"voice2text":{"new":[..]}}` (1 s timer, `take_new_message`) and
  `{"voice2text":{"status":{..}}}` when it changed (state off | starting |
  ready | busy | error | not installed | no model, error, installed,
  models [{name, mb, multi, note}], catalogue (MODEL_CATALOGUE = setup.sh's
  CATALOGUE - keep them equal) with have, vad_model, settings, model_multi,
  log{enabled}, loaded, vad_loaded, queue, queued_s, dropped, done, nospeech,
  failed, rtf, clips, clip_mb, clip_dir (absolute), clip_days, clip_free_mb,
  clip_low, clips_skipped, clip_error, loop_max_ms + in_dropped_s (the
  worker's longest loop / input dropped at the 3 s cap - diagnostics),
  vfos [{id, src, rec, secs, digital}],
  download{name, running, pct, error}); `GET_VOICE2TEXT` (page connect) ->
  status + `history` (300). `VOICE2TEXT_CLEAR` empties history, file and
  clips. Settings (schema BEFORE DECIMATORS, so restored VFOs start with
  the model set): `VOICE2TEXT_MODEL:` (name = ggml-<name>.bin, [A-Za-z0-9._-],
  no leading dot), `_LANGUAGE:` (auto | code), `_THREADS:1-4` (default 2),
  `_PROMPT:`, `_VAD:`, `_TRANSLATE:`, `_KEEP_AUDIO:`; `VOICE2TEXT_VFO:`,
  `_DOWNLOAD:`, `_CLEAR` are in `is_query_command` (own broadcasts)
- `VOICE2TEXT_DOWNLOAD:name`: catalogue names only (fixed Hugging Face
  URLs), `curl` via posix_spawnp on a detached thread -> `.part` -> rename
  (+ the VAD model when missing); progress = file size / catalogue MB
- Paths: `KRAKEN_TRANSCRIBE_DIR` (default `transcribe`, cwd-relative) holds
  `build/kraken_whisper`; models in `KRAKEN_WHISPER_MODELS` (default
  `<dir>/models`). Docker: helper built in the image (`--build-arg
  WHISPER=0` = without), models in `/data/whisper_models` (entrypoint), curl
  in the runtime image
- Clips ("Keep the audio", default on; `save_clip` / `write_opus`, on the
  worker): Ogg Opus (RFC 7845: OpusHead + OpusTags, libopus - already linked
  for the live audio - 16 kHz mono VOIP 16 kbit/s, complexity 8, pre-skip =
  lookahead x 3, the last page's granule trims the padding; Ogg CRC by hand;
  ~2 kB/s = ~7 MB per hour of speech vs 115 MB as WAV) with Vorbis-comment
  tags TITLE (the text), DATE, KRAKEN_VFO / MHZ / SOURCE / TALKER / BEARING.
  In the DECODER LOG's folder, also with logging off: `declog::folder()` +
  `/voice_clips/YYYY-MM-DD/<id>.opus` (local date; .tmp + rename).
  `clips_tick` (2 s): a changed folder = rescan (`scan_clips`: only
  `<digits>.opus` in YYYY-MM-DD dirs; clips in the old folder stay there,
  no longer in the index - the tab says "the audio is gone"); retention
  (`clip_retention`: day dirs older than `declog::keep_days()`, 0 = keep;
  clip-named files only, then rmdir - a foreign file keeps its dir) every
  10 min and when the days / date change; free space (statvfs of the
  nearest existing parent) < `MIN_FREE_BYTES` 100 MB -> `clip_low`, clips
  skipped + counted (transcripts go on); `MAX_CLIP_BYTES` 2 GB backstop,
  oldest first. The log record's `clip` is relative to the log folder
- HTTP `GET /voice_clip/<id>.opus` (websocket_server, `audio/ogg`): host
  check + token like /recordings; only ids in the clip list
  (`v2t::clip_path`)
- UI (kraken_doa.html "🗣 VOICE TO TEXT" block; state `v2tStatus / v2tList /
  v2tUnseen` in the globals block - `infoRenderTabs` reads them): sidebar
  box `#v2t-section` (`v2tRenderBox`: install hint, model + ⬇ Get list,
  language row only for multilingual models, cores + speed, prompt, VAD /
  keep ticks, status, "To disk" + `v2tLogOn` = DECODER_LOG_TYPES + voice,
  DECODER_LOG:1), VFO cards (`dec-v2t-<id>` + state, `v2tRenderCards` from
  updateDecimatorList and every status), info panel tab `infoTab = 'voice'`
  (`#info-body-voice`, `v2tRenderTab`: newest first, 400 rows, search with
  `<mark>` on the raw text, VFO filter, ▶ `v2tPlay` - fetch with the token
  -> blob -> Audio; a browser without Ogg Opus (`canPlayType`, older
  Safari): `v2tDecodeOgg` (Ogg pages -> packets, pre-skip from OpusHead ->
  the live audio's opus-decoder.min.js) -> WebAudio; the box's clip line
  `#v2t-clips` (count, MB, Keep days, folder, low disk / errors), ⬇ CSV `v2tExport` - cells starting = + - @ get a ' so a
  spreadsheet doesn't run them, Clear), tab label with a red dot while
  recording and a count of transcripts that came while hidden
- Lobes on the 🗺 map (kraken_doa.html, in the "🗣 VOICE TO TEXT" block;
  state `v2tLobes` (id -> {L, col, peak, el}) / `v2tLobeWant` in the globals
  block - mapDrawNow -> `v2tLobeDraw` runs before the section is
  evaluated): a row's 🧭 bearing (`.v2t-brg.lb`, `lb` set) -> `v2tLobeClick`
  (again = hide) -> VOICE2TEXT_LOBE -> `v2tLobeAdd`: colour = the first of
  `V2T_LOBE_COLS` (white, pale violet / blue / green / orange / red - none of
  the VFO colours, `getDecimatorColor`) no other shown lobe has, the map
  opened (`mapToggle` / `rpShow('map')` from the DoA tab), centred on the
  place when it is off screen. `v2tLobeDraw` (after `rdfDraw`): from
  slat / slon, else the station now (`mapStationPos`); `rdfLobePath` at the
  live lobes' radius (0.3 of the map), fill 0.16, dark edge + the colour
  DASHED (live lobes: solid in the VFO's colour); a tag (`.v2t-ltag` in
  `#map-v2t` over the canvas, made once per lobe, moved every draw: past
  the tip along the peak, kept inside the map) "● D0 17:18:19 · 🧭 60°"
  ("arr 90° (array)" for an `lf` "a" lobe - drawn with the array's front
  north) + ✕ (`v2tLobeRemove`); its pointerdown doesn't reach the map (the
  map listens on the canvas). The row's bearing is outlined in the lobe's
  colour while shown; a history without the id (Clear) removes it
- Measured (Pi 5, other sessions loading it, load avg 5-8): base.en ~280 MB
  RSS, an 8-11 s transmission in 3-7 s on 2-3 threads (rtf 0.3-0.6),
  tiny.en 175 MB / ~2x faster but poor on radio audio, small.en-q5_1 460 MB
  and ~2x slower than real time (the most accurate). `audio_ctx` (shorter
  encoder window) was SLOWER here and is documented to cost accuracy - not
  used. Parakeet (also in whisper.cpp 1.9) not used
- Tests (scratchpad of job 27e7efb6): fake_heimdall_voice.py (FM with a 100
  Hz CTCSS / AM / bare carrier speech clips onto a 5-element UCA from set
  bearings, generated block by block) + a TEST build on 3908x + t_v2t.py:
  D0 NBFM squelch on (JFK 60 deg, Apollo-13 200 deg, a 1.5 s carrier ->
  "no speech", fast speech 300 deg), D1 AM squelch off (auto gate,
  overlapping D0), D2 not ticked: every text as expected, doa exact (300 /
  160 / 60 / 240), D2 nothing, decoder log + clips + snapshot field,
  refused commands (`../../etc/passwd` model, threads 9, `EN;rm`, unknown
  download, VFO 9), clip route 200 / 404s. Digital: the scratch plugin
  ptest/voicetest (scripted decoded voice + talkers through the real plugin
  protocol): unit 1234567 -> TG 100 11.0 s, 7654321 right after (split at
  the change), "?" then 5550001 (adopted) - texts right, from / to in the
  log; call 2 named only 0.6 s in -> call 1 exactly 11.0 s; `VT_MODE=dmr`:
  slot 2's radio keys up during slot 1's call (talker with a later end,
  audio played after) -> 1001 one 11.0 s transcript, 2002 its own.
  run_dl.sh: empty models folder -> "no model", `VOICE2TEXT_DOWNLOAD:tiny.en`
  (progress, the VAD model too, a 2nd refused while running) -> model ->
  ready (the page that pressed ⬇ Get switches to it when the chosen model
  isn't installed, `v2tAutoPick`). Corrupt history lines skipped; a scene
  cut mid-transmission -> its 6.3 s transcribed 2 s later. t_ui.py
  (headless Chromium, 34 checks): box, cards, tab, search, filter, escaping
  of injected markup, CSV formula guard, clip play / stop (native Ogg Opus
  and the forced decoder fallback - the whole clip decoded), "audio is
  gone", the clip line, toggles, settings, tab kept over a reload. No P25
  voice recording here and no mbelib (DMR / D-STAR / NXDN): real vocoders
  not tried. Cutting + clips (make_bc.sh / run_bc.sh, 3 antennas): a 36 s
  WBFM station (JFK + Apollo + Micro Machines, no pre-emphasis in the
  generator) -> 5.5 / 11.9 / 7.9 / 11.4 s pieces, the first cut at JFK's
  pause ("... ask not" | "what your country ..."); a 25 s two-way -> 22.2 +
  3.4 s at a pause, a 20 s one -> ONE transcript; Opus files = transcript
  dur (ffprobe), retention of a planted 12-day-old folder (its foreign file
  + folder kept), relative clip paths in the log. NB the generator's weak
  carrier (0.05 in 0.02 noise, ~300 Hz rms deviation: ~17 dB audio SNR)
  made the two-way unintelligible - not the code: 10 dB more matched the
  source offline. run_clips.sh: DECODER_LOG_DIR mid-run -> new clips + log
  in the new folder; a build with MIN_FREE_BYTES 2^60 -> no clips,
  clips_skipped 1..4, transcripts on. Lobes (run_lobe.sh + t_lobe.py):
  the voice scene's 4 transcripts each with a 720-bin north lobe peaking at
  its bearing (300 / 240 / 160 / 60, <= 0.1 deg), the station position;
  pushes without lobe data; a bad id refused, an unknown one -> error; after
  a restart (no heimdall) all 4 back from transcripts.jsonl.
  run_lobe_ui.sh + t_lobe_ui.py (headless Chromium, 20 checks, live D0 / D1
  lobes on the map at the same time): bearings clickable, click -> map opens,
  peak = bearing, tag + ✕, row marked, colours unlike the VFOs', drawn
  along the peak and nothing opposite (on a canvas of their own - the map's
  holds cross-origin tiles, unreadable), a second lobe in another colour,
  ✕ / a second click remove, an array-frame lobe tagged "(array)", error
  answer, another page's answer ignored, Clear removes them

