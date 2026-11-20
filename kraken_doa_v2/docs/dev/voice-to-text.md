# Voice to Text

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**🗣 Voice to Text (`src/voice2text.cpp`, `include/voice2text*.hpp`, `transcribe/`, ⚙ Setup Settings → "🗣 Voice to Text", the 🗣 Transcription panel):**
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
  ignored and the queue cleared (no "dropped" spam while not set up). Live
  text's partials never queue: one is sent only while nothing is queued or
  in flight ("Live text" below)
- Helper `transcribe/build/kraken_whisper` (transcribe/kraken_whisper.cpp,
  CMake: whisper.cpp via add_subdirectory, static) started with
  `PluginProcess::start(argv)` (new overload; same pipes / stderr tail as
  plugins): `--serve --model M --threads N [--vad V]`, nice 10 (it renices
  itself). Wire `include/voice2text_wire.hpp` (plugin framing): JOB = id +
  int16 16 kHz, OPTION key=value (language, prompt, threads, vad,
  vad_threshold, translate, beam), READY (model, type, multilingual, vad,
  version, system, live), RESULT (id, conf, no_speech, proc_ms, speech_ms, lang,
  text, dropped reason), ERROR; PARTIAL 3 / PARTIAL_RESULT 19 for Live
  text (2026-11-20, "Live text" below). ONE job at a time (the next is sent after
  the RESULT; a PARTIAL counts as that one job); options re-sent when they change; a model change restarts
  it. Crash -> the in-flight job is retried once (a partial is not), restart after 2..30 s; a
  job or partial running > max(90 s, 15x its length) kills it. No VFO transcribing for
  `HELPER_IDLE_STOP_MS` 2 min -> stopped (base.en ~280 MB RAM). The same
  binary makes the derived q8_0 models: `kraken_whisper --quantize IN OUT
  q8_0` (below)
- In the helper per job: Silero VAD (min speech 200 ms, min silence 300 ms,
  pad 200 ms) -> only the speech parts, joined by 150 ms of silence (< 200
  ms of speech = "no speech", whisper never runs: 30-40 ms for 5 s of noise)
  -> whisper_full (greedy, no_context, no_timestamps, suppress_nst, language
  "en" for .en models; since 2026-11-20 a SIZED encoder window, below) ->
  segments with no_speech > 0.6 AND avg logprob < -1
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
  NO speech, so speech_ms couldn't be read.
  ENCODER WINDOW (2026-11-20, `encoder_window()`): whisper encodes 30 s
  whatever the input - ~90 % of a 5-13 s job. `audio_ctx` = the input (the
  joined speech) in 20 ms frames + 50 (1 s more), rounded UP to a multiple of
  256 (5.1 / 10.2 / 15.4 / 20.5 / 25.6 s; from 24.6 s of input on the full
  1500). The input's length exactly made whisper loop (no end-of-text ->
  ~220 tokens -> 5 temperature fallbacks: slower than 30 s - why it "was
  SLOWER" before); + 1 s unrounded gave texts that depended on the clips
  decoded BEFORE (whisper.cpp 1.9.4 with flash attention, its default, reads
  the cross-attention cache in steps of 256 frames, unmasked: past
  audio_ctx it read what an earlier encode had left - zeros in a fresh
  process; 5 of 15 clips changed between run 1 and 2, loops came and went).
  Rounded, every frame read is this input's: the same text in any order and
  with any thread count. A full-window decode runs in a whisper state no
  sized encode has written into (`state_for`: one state at a time, a fresh
  one - caches zeroed by whisper.cpp, 4-24 ms - when the other kind is
  needed), so its padding is zeros as before; two states at once took
  base.en 259 -> 322 MB, small.en 662 -> 789 MB peak, one at a time 262 /
  663 MB (sized jobs only: 228 / 623 MB). LOOP GUARD: a token cap through `logits_filter_callback` - 6 text
  tokens per second of input (multilingual 10) + 16; at the cap end-of-text
  is forced (and at once in whisper's temperature fallbacks); a final decode
  that reached it is decoded again with the full window and no cap = the
  decode of before, so the cap can never cut a transcript short (a low cap
  only costs CPU; tested with a build whose cap fired on every decode: 28 of
  30 texts = the old helper's, the other 2 never reached it). Applies to
  every decode (the no-prompt and two-halves retries too); temperature
  fallback kept for finals. With the rounded window no English model's
  decode reached the cap on the bench clips; the multilingual base did once
  (a clip it took for German: 10.5 CPU s with the guard, 6.3 s in the old
  helper - its own temperature fallbacks).
  PARTIAL (Live text, wire type 3 -> PARTIAL_RESULT 19, READY `live=1`;
  `transcribe_partial`): the same VAD (< 200 ms speech -> "" without
  whisper), prompt, language, sized window and clean-up, but decoded ONCE:
  greedy even with a beam set, `temperature_inc` 0, the token cap just ends
  it (no retry), no no-prompt / two-halves retries, and a phrase of up to 8
  words looped 4+ times kept once with a cut-off copy at the end dropped
  (`collapse_phrase_loops` - the cap often stops a loop mid-phrase: "ask not
  ask not ... ask"). Without the guards whisper looped / invented words on
  half-spoken audio (7-10 s decodes); with them 0.17 s median, 0.53 s max
  (base.en-q8_0, 2 threads; small.en-q8_0 4 threads 0.38 / 1.2 s). Offline:
  `--file x.wav --partial` (one partial), `--stream 1000` (the live
  schedule: a partial of the audio so far every second, LocalAgreement-2
  confirmed / pending per line, then the final and a summary).
  `kraken_whisper --quantize IN OUT q8_0|q5_0|q5_1` (`transcribe/quantize.cpp`,
  ported from whisper.cpp's quantize example, MIT): byte for byte what
  whisper-quantize writes (checked: base.en / small.en q8_0), ~20 MB of
  memory (1M weights at a time), refuses a quantized / non-speech-model /
  truncated input and OUT = IN, removes OUT on failure, exit 0 = done.
  Helper tests 2026-11-20 (job fa22c145 scratch, `helper/`): t_wire.py (23
  checks on the wire: READY live=1, JOB / PARTIAL answers, silence / noise /
  empty partials -> "", OPTIONs, unknown types and short jobs ignored, 19 /
  27 / 44 s inputs decoded to their last words, oversize -> ERROR + exit 2,
  a bad model -> ERROR), t_guard.sh (above), t_quantize.sh (19 error paths:
  wrong type / input, truncated, OUT = IN also by sym / hard link, /dev/full),
  t_setup.sh (11, a stub curl: the derived models with / without the f16,
  no helper, a failing helper), bench_serve.py / wer.py / stream_stats.py
  (the numbers below)
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
- Set up or not is the card at the top of ⚙ Setup Settings → 🗣 Voice to
  Text (`v2tRenderSetup`, from the same status; the "not installed" text is
  `v2tInstallHtml()`, per receiver OS / Windows package). It was a card of
  that dialog's Add-ons tab, beside the sidebar box's own note, until
  2026-11-17 - *sidebar.md*, "Setup Settings dialog".
- Pushes: `{"voice2text":{"new":[..]}}` (`take_new_message`) and the live
  rows `{"voice2text":{"live":[..]}}` when they changed
  (`take_live_message`, "Live text" below) on a 250 ms timer (1 s until
  2026-11-20; the new transcripts first, so a final replaces its live row
  in the same pass), and once a second
  `{"voice2text":{"status":{..}}}` when it changed (state off | starting |
  ready | busy | error | not installed | no model, error, installed,
  models [{name, mb, multi, note}], catalogue (MODEL_CATALOGUE = setup.sh's
  CATALOGUE - keep them equal) with have and, for a derived model, from
  (below), vad_model, settings (+ live), model_multi, live_ok (null = no helper
  loaded yet, false = an older helper), partials, partials_stale, partial_ms,
  log{enabled}, loaded, vad_loaded, queue, queued_s, dropped, done, nospeech,
  failed, rtf, clips, clip_mb, clip_dir (absolute), clip_days, clip_free_mb,
  clip_low, clips_skipped, clip_error, loop_max_ms + in_dropped_s (the
  worker's longest loop / input dropped at the 3 s cap - diagnostics),
  vfos [{id, src, rec, secs, digital}],
  download{name, running, stage (download | quantize), pct, error}); `GET_VOICE2TEXT` (page connect) ->
  status + `history` (300) + `live` (the rows now). `VOICE2TEXT_CLEAR` empties history, file and
  clips. Settings (schema BEFORE DECIMATORS, so restored VFOs start with
  the model set): `VOICE2TEXT_MODEL:` (name = ggml-<name>.bin, [A-Za-z0-9._-],
  no leading dot), `_LANGUAGE:` (auto | code), `_THREADS:1-4` (default 2),
  `_PROMPT:`, `_VAD:`, `_TRANSLATE:`, `_KEEP_AUDIO:`, `_LIVE:0|1` (default 0,
  anything else refused); `VOICE2TEXT_VFO:`,
  `_DOWNLOAD:`, `_CLEAR` are in `is_query_command` (own broadcasts)
- `VOICE2TEXT_DOWNLOAD:name`: catalogue names only (fixed Hugging Face
  URLs), `curl` via posix_spawnp on a detached thread -> `.part` -> rename
  (+ the VAD model when missing); progress = file size / catalogue MB.
  DERIVED models (2026-11-20, `CatalogueEntry::from`: base.en-q8_0 78 MB,
  small.en-q8_0 252 MB - whisper.cpp publishes no English-only q8_0 file):
  made here, never downloaded as such. `download_thread` runs steps
  (`DlStep`): the f16 model from its own fixed URL unless installed - then
  into `ggml-<name>.f16` (no model list shows it; deleted afterwards, also
  when quantizing failed; an installed f16 model stays) -, then
  `<transcribe dir>/build/kraken_whisper --quantize <f16> <models>/ggml-<name>.bin.part
  q8_0` (kproc, nice 10, stderr kept: a failure reports its last line,
  "making the model failed (exit 3): ...") and the rename, then the VAD
  model if missing. stage "quantize" while it runs, its pct = the output's
  size / the catalogue MB. Refused without the helper ("the speech helper
  is not installed - it makes this model"); `v2t::stop()` ends the
  quantizer as it does curl
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
  (`clip_retention`: day dirs older than `declog::keep_days()` - Decoder
  Logging's Keep, the plain days; Then compressed does not apply to clips,
  which are Opus already (2026-11-19) -, 0 = keep;
  clip-named files only, then rmdir - a foreign file keeps its dir) every
  10 min and when the days / date change; free space (statvfs of the
  nearest existing parent) < `MIN_FREE_BYTES` 100 MB -> `clip_low`, clips
  skipped + counted (transcripts go on); `MAX_CLIP_BYTES` 2 GB backstop,
  oldest first. The log record's `clip` is relative to the log folder
- HTTP `GET /voice_clip/<id>.opus` (websocket_server, `audio/ogg`): host
  check + token like /recordings; only ids in the clip list
  (`v2t::clip_path`)
- UI (kraken_doa.html "🗣 VOICE TO TEXT" block; state `v2tStatus / v2tList /
  v2tUnseen` in the globals block - `infoRenderTabs` reads them): the
  🗣 Voice to Text tab of ⚙ Setup Settings (`.set-pane[data-t="v2t"]`; a
  sidebar box, `#v2t-section` / `data-sb` "voice-to-text", until 2026-11-17,
  when the user asked for everything about setting the software up to be in
  that dialog - the ids stayed, `v2tRenderBox` kept its name: the set-up
  card `#v2t-card` (`v2tRenderSetup`, the box's `?` in its header), model +
  ⬇ Get list, language row only for multilingual models, cores + speed,
  prompt, the Live text switch (2026-11-20, "Live text" below), VAD / keep switches, status rows, "To disk" (since 2026-11-19
  "decoder log files" there is a link to the Decoder Logging section of the
  same dialog, amber with a ⚠ while the log has a problem - *decoder-log.md*;
  the Transcription panel's head says "Decoder log: not writing" etc. with a
  link while transcripts are logged; while off: "turn on" sets the
  transcripts' Logging to Log, "not allowed" goes to the Decoder Logging
  section; an older receiver: `v2tLogOn` = DECODER_LOG_TYPES + voice,
  DECODER_LOG:1, as before); the box header's badge is a ⚠
  on the tab, `#set-tab-v2t-n`, while VFOs are ticked and the state is not
  installed / no model / error - its VFO count is the VFOs row; the box's
  "⤢ Transcripts" button went, the rail opens the panel), VFO cards
  (`dec-v2t-<id>` + state, `v2tRenderCards` from updateDecimatorList and
  every status; the "⚠ Not installed" / "⚠ Error" state is a link,
  `a.v2t-go` -> `setDlgOpen('v2t')`, as is "⚙ Setup Settings" in the
  panel's warning, `v2tRenderHead`), the 🗣 Transcription panel - the
  panel under the waterfall with `infoTab = 'voice'`. It is a feature of its
  own there, not a tab beside the decoders' (it was one, "🗣 Voice", until
  2026-11-17; the user: it is not a decoder): the rail's 🗣 Transcription
  button (`#rail-voice`, `railPanel('voice')`) opens the panel with the
  transcripts alone, under the name "🗣 Transcription" in place of the tab
  strip (`infoRenderTabs`), and 🔐 Decoders shows the decoders' tabs without
  it - *sidebar.md*, "Top bar and rail".
  (`#info-body-voice`, `v2tRenderTab`: the live rows first ("Live text" below), then newest first, 400 rows, search with
  `<mark>` on the raw text, VFO filter, **Logging: Off / Log** after it
  (2026-11-19, the user: "Add a logging drop down to transcriptions too";
  `#v2t-lsel`, `v2tRenderLogSel` / `v2tSetLog`: the transcripts to the
  decoder log or not - the `voice` member of DECODER_LOG_TYPES, stored as
  before; written to `<log folder>/transcripts/transcripts-YYYY-MM-DD.jsonl`
  since 2026-11-19, every source in that one folder - *decoder-log.md*; no Log All, transcription has no repeated messages; greyed with
  a Decoder Logging button beside it while logging to disk is not allowed -
  *decoder-log.md*), ▶ `v2tPlay` - fetch with the token
  -> blob -> Audio; a browser without Ogg Opus (`canPlayType`, older
  Safari): `v2tDecodeOgg` (Ogg pages -> packets, pre-skip from OpusHead ->
  the live audio's opus-decoder.min.js) -> WebAudio; the tab's clip line
  `#v2t-clips` (count, MB, Keep days, folder, low disk / errors), ⬇ CSV `v2tExport` - cells starting = + - @ get a ' so a
  spreadsheet doesn't run them, Clear), the panel's name with a red dot while
  recording; the count of transcripts that came while the panel was closed
  or showed the decoders is on the rail's button (`#rail-voice-n`, `railSync`)
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
  and ~2x slower than real time (the most accurate) - all with the full 30 s
  window of before 2026-11-20. The sized window (above; measured 2026-11-20 on
  the PC, i7-11800H, 14 WBFM speech clips + JFK, the helper's serve mode, CPU
  s per job, word error rate against medium.en-q5_0 with the full window):
  base.en 2 threads 2.91 -> 0.93 s (11.1 % -> 11.1 %), small.en 2 threads
  10.8 -> 3.65 s (9.3 -> 7.4 %), 4 threads 12.1 -> 4.09 s; 8-bit q8_0 models
  (made with `--quantize`): base.en-q8_0 2.15 -> 0.78 s (10.6 -> 12.5 %: one
  clip's last sentence twice), small.en-q8_0 8.29 -> 2.67 s (9.3 -> 7.4 %) -
  q8_0 ~16 % (base) / 27 % (small) less CPU than f16, about as accurate;
  q5_1 was slower than f16 on this PC. The multilingual base (language
  auto): 5.66 -> 2.2-2.8 s (13.9 -> 15.3 %); its texts the same forwards and
  backwards except one clip that needs whisper's temperature fallback
  (random sampling - the old helper's results depend on the jobs before it
  there too). Threads: base.en 1 / 2 / 4 = 0.89 /
  0.93 / 1.09 CPU s per job (0.88 / 0.47 / 0.28 s wall): more threads finish
  sooner but cost more CPU. Every text identical over 2-3 runs and across
  thread counts. Live text (`--stream 1000` on the 15 clips): base.en-q8_0 2
  threads partial decodes 0.17 s median / 0.53 s max, 0.43 cores while
  someone talks, words shown 0.8 s (median) / 1.2 s (p90) after they were
  spoken, confirmed 1.8 / 3.3 s; base.en 2 threads 0.58 cores, 1.0 / 1.3 s,
  2.0 / 3.5 s; small.en-q8_0 4 threads 1.91 cores, 1.3 / 1.6 s, 1.9 / 2.7 s
  (2 threads 1.48 cores, 1.8 / 2.5 s, 3.0 / 3.9 s); small.en 4 threads 2.56
  cores, 1.5 / 1.9 s, 2.5 / 4.1 s; 51-63 % of the words confirmed before the
  final, 3.5-7 % of those changed by it. Not measured on a Pi (estimate x3-5).
  Parakeet (also in whisper.cpp 1.9) not used
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

## Live text (2026-11-20)

The user asked (job fa22c145): "keep the audio clips as they are now, and
just implement the CPU enhancements and live transcription option". Live
text shows what is said while a transmission is still on the air; the
final transcript - and with it the clip, the history, the log - is made
and looks exactly as before, and replaces the live text. An OPTION, off by
default, because of its CPU cost (measured below).

- Setting: `VOICE2TEXT_LIVE:0|1` (`v2t::set_live`, persisted as
  `voice2text_live` after `_KEEP_AUDIO` - before DECIMATORS -, default 0;
  any other value refused), in the status as `live`; `live_ok` = the
  running helper's READY said `live=1` (null before one loaded, false = an
  older helper: then no PARTIAL is ever sent - it would ignore the unknown
  type and never answer - and the page says "not with this helper").
- Scheduling (`Worker::maybe_partial`, every worker pass): each open piece
  (`Tap::piece`, a key per piece - a new one at a transmission's start, at
  every cut into parts and at a talker split) is DUE when it holds
  `LIVE_FIRST_S` 1.0 s (its first partial), later when `LIVE_STEP_MS` 1 s
  passed since its last partial was sent AND `LIVE_NEW_S` 0.5 s more audio
  came; never while its analog squelch is closed (the transmission is
  ending: its final follows within `HANG_S`, and a partial then only
  delayed it by its own run time - the first test run showed it). The
  audio is what the final would get if the piece ended now
  (`Tap::live_len`: pre-roll included, the audio since the squelch closed
  cut to the tail `finish` keeps). The piece due longest (`Tap::live_due`)
  goes, and only while no final is queued (`pending_`) and nothing is in
  flight: finals always first, one message in flight, so several VFOs
  talking at once alternate and each gets fewer updates. Not during a
  playback (`playback::active()`; the input stops then anyway).
- PARTIAL_RESULT (`Worker::on_partial`): discarded (counted
  `partials_stale`) when its piece was finalized, dropped or replaced
  meanwhile (the tap's piece key moved on) or Live text went off; a piece
  whose partials are all "" (no speech yet) gets no row. Partials never
  reach the history, transcripts.jsonl, the decoder log, clips, `done` /
  `nospeech` / `rtf`; they count in `partials` and `partial_ms` (moving
  mean of their proc_ms).
- LocalAgreement-2 per piece (`LiveRow::agree`): the latest partial's words
  (`split_words`: whitespace; a token without a letter or digit is kept
  with the word before it, so "-" or "..." can't shift the words after it)
  are aligned to the confirmed words (`align_confirmed`: where its words
  match the last two confirmed ones, with up to two words more or fewer
  before them - whisper put one in or left one out early on -, else at the
  same count: a partial that contradicts the confirmed words leaves them
  and shows its rest as pending); the common prefix of what follows and
  the previous partial's rest (compared by `norm_word`: ASCII letters
  lower-cased, digits and UTF-8 bytes kept, punctuation dropped) is added
  to the confirmed words, as the latest partial wrote them; confirmed only
  grows; pending = the rest of the latest partial.
- Rows (`Worker::live_`, piece key -> `LiveRow`): made by the first
  partial with text, updated by each (talker / label, part, seconds of
  audio, confirmed, pending); `open` false once the piece ended (its final
  queued); gone when its final RESULT comes (in the same st_mu section as
  the transcript joins the `new` list - also for "no speech" and a result
  dropped by a Clear), when its job is dropped (queue overflow, the helper
  unusable, failed twice), when it was shorter than `MIN_UTTERANCE_S`, and
  all of them when Live text goes off. A long transmission's parts each
  get a row of their own (part = the transcript's part number).
- Push: `{"voice2text":{"live":[{vfo, key, t (the piece's start, ms), mhz,
  src, tid, tl, part, secs, open, confirmed, pending}]}}` - the WHOLE list
  each time it changed (`s_.live_json` + `live_seq`, built under st_mu by
  `live_publish_locked`), from the 250 ms Voice to Text timer after the
  `new` transcripts, so the final and the row's removal arrive in the same
  pass; `GET_VOICE2TEXT` also answers `live_message()` (a page connecting
  gets the current rows). An older page ignores the key; an older receiver
  refuses the command (unknown).
- Page: the switch "Live text" in Setup Settings → Voice to Text after
  "Words to expect" (`#v2t-live` in `#v2t-live-l`; its `?` tip has the
  costs below in plain words - a tap on it doesn't flip the switch, the
  help handler eats the click), hidden for a receiver without `live`; drawn
  from the status only (no optimistic state). Status row "Live text"
  (`#v2t-live-row`, shown while on): "N updates · 0.4 s each", or "not
  with this helper" with a warning icon (title: update it with `make
  whisper`). The rows: `v2tLive` (globals block), `v2tRenderLive` into
  `#v2t-live-rows` above `#v2t-rows` in the Transcription panel's list
  (`v2tRenderTab` calls it first; the VFO filter - whose options include
  VFOs with live rows only - and the search apply; newest first),
  `v2tLiveHtml`: the `mic` icon + "Live" in `--bad-t` with a 2 px `--bad`
  left edge (the panel's recording red), `hourglass` + "Finishing" with an
  `--l4` edge once the transmission ended, the time, VFO, MHz, source,
  talker, seconds (+ part), the confirmed text as a transcript's (`--t0`),
  the pending text `.pend` in `--t5` (title "Not confirmed yet"), both
  escaped (`v2tMark`, the search marks too). No clip, bearing, export or
  unseen count - a live row is not a transcript. Someone reading older
  rows lower down keeps them in place while the live rows change height
  (`scrollTop` moved by the difference; `.v2t-list` has
  `overflow-anchor:none` - Chrome's own scroll anchoring on top of it
  moved them twice, and Safari has none), and since 2026-11-20 also when
  a new transcript comes in above them (`v2tRenderTab` re-finds the row
  that was at the top of the view by its `data-id`; it used to keep
  `scrollTop`, so the list moved down a row per new transcript - with a
  final replacing its live row the two together moved it). The empty "No
  transcripts yet" (`.v2t-empty`) hides while live rows show.
- Measured (job fa22c145, this PC = i7-11800H, 14 real WBFM voice clips +
  jfk.wav). The design came from a bench simulation (re-decode the whole
  transmission so far every step, LocalAgreement-2, the final at the end
  + 0.8 s hang as today): partial decodes NEED guards - without them
  whisper looped / hallucinated on half-spoken audio (7-10 s decodes,
  invented words confirmed); with VAD on the buffer, no temperature
  fallback and a token cap they took 0.2-0.6 s (the helper's PARTIAL path,
  above). A 0.5 s step: words only 0.3 s sooner for 1.7x the CPU, and 45 %
  of the confirmed words changed later - so 1 s. The helper as built
  (`--stream 1000`, the "Measured" bullet above): base.en-q8_0 2 threads
  words shown 0.8 / 1.2 s after they were spoken (median / p90),
  confirmed 1.8 / 3.3 s, 0.43 cores while someone talks; base.en t2 1.0 /
  1.3, 2.0 / 3.5, 0.58; small.en-q8_0 t4 1.3 / 1.6, 1.9 / 2.7, 1.91;
  small.en t4 1.5 / 1.9, 2.5 / 4.1, 2.56 cores. End to end through
  kraken_doa (tests below; two NBFM VFOs, 33.5 s of speech in a 60 s
  scene, the two talking at the same time for most of it): the helper's
  CPU per scene base.en 4.4 s with Live text off, 25.5 s on (5.8x);
  base.en-q8_0 3.2 / 19.8 s (6.2x) - the old helper (full window, no Live
  text) 14.8 s; the finals the same with it on or off. Pi 5 (this PC x3-5,
  NOT measured): base.en-q8_0 words ~1.5-2 s behind, 1.5-2 of its 4 cores
  while someone talks - hence an option, off by default.
- Threads: more finish each transcript sooner but cost more CPU in total
  (the helper with the sized window, base.en 1 / 2 / 4 threads: 0.89 /
  0.93 / 1.09 CPU s per job, 0.88 / 0.47 / 0.28 s wall; the bench with
  small.en: t1 2.73, t2 2.58, t4 3.27, t8 4.86 CPU-s) - the CPU cores
  row's tooltip says so (2026-11-20: "1 or 2 use the least CPU in total" -
  2 the bench's minimum with small.en, 1 and 2 within 5 % with base.en);
  the default stays 2.

## Derived models: base.en-q8_0, small.en-q8_0 (2026-11-20)

whisper.cpp's quantize (q8_0, from the f16 model) used ~16 % (base.en) /
27 % (small.en) less CPU than f16, about as accurate (the "Measured"
bullet above); q5_1 was slightly slower than f16 here and a little less
accurate. whisper.cpp publishes no English-only q8_0 files, so the
two are made on the receiver: `CatalogueEntry::from` in MODEL_CATALOGUE
and the same two names / sizes in setup.sh's CATALOGUE (keep the lists
equal), made by the page's Get (`VOICE2TEXT_DOWNLOAD`, above) or
`transcribe/setup.sh base.en-q8_0`, both with the helper's `--quantize`
(one binary: Docker and the Windows package copy only
`transcribe/build/kraken_whisper[.exe]`). The default model stays base.en.
Not measured on a Pi.

## Tests (job fa22c145, scratch `client/`)

A TEST build (`sync_tb.sh`: a copy with the ports on 2858x / 2859x, exe
`kraken_doa_test`), `fake_heimdall_v.py` (FM transmissions on a schedule,
generated packet by packet: tones or 16 kHz WAV clips, any length),
`stub_whisper.py` as `build/kraken_whisper` in a scratch
`KRAKEN_TRANSCRIBE_DIR` (the wire protocol without a model: the tone picks
a script, the audio length how many words were said; a partial's last word
garbled differently each time, so it must never be confirmed; READY with
or without live=1; delays; a reader thread logs a message that arrives
while another is being worked on; `--quantize` copies the file + a
marker), a fake `curl` on the PATH (no network).
- t_live.py (31 checks): one VFO - the first partial at 1.00 s of audio,
  then exactly 1.00 s apart with >= 0.5 s new audio, never two messages in
  flight, confirmed only grows (0, 0, 3, 5, 8, 10, 13, 15 words), nothing
  garbled ever confirmed, the confirmed words are the final's beginning,
  the row goes in the push of its final, unchanged after its transmission
  ended; two VFOs at once - partials strictly alternate
  (ABABABABABABABABAB), both rows in one push; two finals queued at once
  while a third VFO talks (slow finals) - job, result, job with no partial
  between; a 32 s transmission - cut at 22.7 s, a row per part (part 1
  closed, part 2 fresh), partials of the ended part discarded (stale 2 with
  1.6 s partials); an older helper - no PARTIAL, live_ok false, no rows;
  the switch off -> none, on mid-transmission -> partials, off again ->
  rows cleared at once; `VOICE2TEXT_LIVE:2` / `:` / `:yes` / `:01`
  refused, `:1` saved and restored after a restart; a page connecting
  mid-transmission gets the rows; transcripts.jsonl lines = finals =
  `done`, no partial text in the history, finals or decoder log.
- t_dl.py (25 checks): base.en-q8_0 with base.en installed - quantize only
  (`--quantize <f16> <out>.part q8_0`, the output renamed, base.en kept,
  nothing downloaded), catalogue have / from / mb, "already downloaded";
  small.en-q8_0 without small.en - the f16 from its fixed URL into
  `ggml-small.en-q8_0.f16`, progress download then quantize, the VAD model
  after it, the f16 deleted, a second download refused meanwhile; a failing
  quantizer - "making the model failed (exit 3): ...: not a ggml model",
  nothing left; a failing download - "download failed (curl exit 22)",
  nothing quantized or left; no helper - refused, a plain model still
  downloads.
- t_ui.py (headless Chromium, 37 checks): live rows appear (two VFOs,
  newest first), mic + "Live", pending in `--t5`, markup in the text
  escaped (`<img onerror>` inert), the VFO filter and search apply, the
  rows go when the finals come and the final rows take their place, no
  page errors; the switch drawn from the status, its `?` tip (CPU cost)
  doesn't flip it, off -> `VOICE2TEXT_LIVE:0` + saved + the status row
  hidden, on again; the cores tooltip; both q8_0 models in the Get list;
  an older helper -> "not with this helper", no rows; Get base.en-q8_0 ->
  "Downloading base.en to make base.en-q8_0…", "Making base.en-q8_0 from
  base.en…", "base.en-q8_0 made", and the page that pressed Get uses it.
- t_ui_scroll.py (headless Chromium): 40 older transcripts, the list
  scrolled to 400 px, two transmissions with live rows coming, growing and
  replaced by their finals - the row at the top of the view stayed put in
  all 64 samples (scrollTop 400 -> 441 -> 483 -> 487 -> 510); at the top
  the list stays at the top.
- End to end with the real helper (the merged worktree-voice2text-live,
  built in scratch from its sources): t_e2e.py - bench clips (JFK 11 s and
  three 7-8 s talk-radio clips) FM-modulated by fake_heimdall_v.py onto D0
  / D1 (NBFM, squelch off), two talking at once: live rows from 1.3 s
  after each piece began (the first partial at 1.0 s of audio incl. the
  0.3 s pre-roll), growing every ~0.5-1 s, 31-32 partials, 0.31 s
  (base.en-q8_0) / 0.42 s (base.en) each, none stale; 14-22 confirmed words
  per transmission, 69 of 70 as in the final with base.en-q8_0, 65 of 66
  with base.en ("American" confirmed, "America" in the final; the words
  at the end of a transmission stay pending until the final: 70 of the
  finals' 80 words were confirmed before it); finals word error rate 3.8 % against
  the bench's reference, the same texts with Live text on or off; CPU as
  in "Measured" above. The main checkout's helper (no live=1) behind a
  logging shim (shim_log.py) with Live text on: 5 OPTION + 4 JOB in, READY
  + 4 RESULT out - no PARTIAL ever -, live_ok false, the 4 finals as
  before. t_e2e_ui.py (headless Chromium, 13 checks): real live rows for
  both VFOs at once (confirmed text, the pending dimmed), replaced by the
  finals, each final with its Ogg Opus clip, in transcripts.jsonl and the
  decoder log (2 records, no partial text); Get base.en-q8_0 with base.en
  installed: "Making base.en-q8_0 from base.en… 93 %" -> "made" in 1.4 s,
  the file byte for byte the helper worker's base.en-q8_0, base.en kept,
  nothing else left.

