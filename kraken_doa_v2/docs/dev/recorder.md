# IQ & audio recorder and playback of recordings

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**📼 IQ & audio recorder + playback of recordings (`src/recorder.cpp`, `src/recorder_audio.cpp`, `src/sigmf.cpp`, `include/recorder*.hpp`, `include/sigmf.hpp`, `src/playback.cpp` RecSource, sidebar "📼 IQ & Audio Recorder"; format: `SIGMF.md`):**
- The user's spec (2026-10-16): record coherent IQ to disk, limited by the
  free space and the projected size; fixed lengths (10 s .. 24 h) and
  Unlimited (until the drive's reserve); OR the audio of one or more VFOs to
  WAV / compressed audio; open recordings and play them back through the
  receiver; SigMF, with the bundling of the per-antenna files chosen here.
  "Local Recording" (the DoA logger, `doa_logger.cpp`) was renamed "📝 DoA
  Recording" in the UI so the two aren't confused (commands / files
  unchanged: LOG_*, doa_recordings/)
- Bundling (decided here, SIGMF.md *Why*): a SigMF COLLECTION - one
  Recording per antenna `<name>_ch<k>.sigmf-meta/-data` + `<name>.sigmf-
  collection` (streams in channel order with each meta's SHA-512) - in the
  folder `<name>/`; transport = ONE SigMF Archive `<name>.sigmf` (POSIX tar
  of that folder, uncompressed) built ON THE FLY by the download (never a
  second copy on the disk) and the only thing an upload takes. Not
  `core:num_channels` (the spec reserves it for audio / scope channels and
  recommends collections for multi-channel IQ; per-antenna files also open
  in every single-channel SigMF tool). Array geometry + calibration bursts:
  the official `spatial` extension (v1.1.0); what the receiver needs to put
  itself back: the `krakensdr` namespace (array commands, VFO snapshot, band,
  station + GPS track, stop reason - SIGMF.md lists every field). Checked
  with sigmf-python 1.13 (venv in the job scratch): every meta validates,
  every core:sha512 + the collection's stream hashes verify, the collection
  loads all antennas; its `fromarchive()` reads one recording per archive
  (a library limit, SIGMF.md says to `tar xf` first)
- Folder: `RECORDER_DIR` (default `recordings`, cwd-relative = the Docker
  volume; gitignored, `*.sigmf` / `*.sigmf-collection` repo-wide) - a
  sub-folder per recording (IQ: the collection; audio: its files) and
  uploaded `<name>.sigmf` archives. Names `iq_YYYYMMDD_HHMMSS_<MHz>`
  (`433_920MHz`), `band_...`, `audio_...` or the page's RECORDER_NAME
  (cleaned to [A-Za-z0-9_-]); `_2`.. on a clash. ONLY recording files
  (`recording_file()`: .sigmf-meta / -data / -collection, .wav, .opus) and
  `.sigmf` archives are listed, served, packed or deleted, and a folder only
  counts when it holds such files - the folder setting can point anywhere
  (like Decoder Logging's), and before that rule a page could set it to a
  parent folder and download `doa_settings.json` / `server.key` as a "recording"
- IQ capture, two kinds (`RECORDER_IQ:full|band`, coherent mode only, not
  during a playback): FULL = heimdall's 8091 packets themselves (cu8 2.4
  MHz - already phase-calibrated by heimdall - 24 MB/s with 5 antennas):
  `rec::on_packet` in the data receiver thread right after the header is
  parsed (before the retune skip: a retuning packet only adds to
  `gap_pending`); BAND = one VFO's decimated block of every antenna
  (`rec::on_band_block` after the decimation barrier; ci16_le, x16384 =
  `krakensdr:iq_scale`, at 2.4 MHz / D; D = 1 refused): while it runs the
  receiver converts every antenna and the pass is NOT fm_only (as with DoA
  on - every VFO decimates every antenna; `rec::band_vfo()`), packet loss
  before the decimation shows as a jump of `RawDataPacket::seq` (new, every
  parsed packet numbered) -> a gap. A VFO bandwidth change stops a band
  recording (SigMF has one rate per file), a channel-count change stops both
- Producer -> `IqSession` queue (chunks, channel-major, each optionally
  opening a capture segment) -> ONE writer thread (detached; it resets
  `g_iq` and signals `g_done_cv` itself - joining it from the uWS loop under
  g_mu deadlocked with its own final g_mu): per antenna `write()`, SHA-512
  (OpenSSL EVP, `sigmf::Sha512`), every 8 MB `sync_file_range` WRITE on the
  new range + WAIT on the one before + `POSIX_FADV_DONTNEED` (bounded dirty
  pages: a 24 MB/s stream otherwise piles up and stalls in bursts). Metadata
  (all metas + the collection, tmp + rename) at the first segment, at most
  every 2 s when a segment / track point was added, final at the end with
  core:sha512 + stop reason + `krakensdr:complete`. Segments: frequency
  (exact: `coherent_tuner_hz()` - new global `requested_freq_hz` set by the
  FREQ handler, the header's float32 is ~64 Hz off near 1 GHz), a gain,
  converged <-> not, the noise flag (`spatial:calibration`), a gap
  (`core:global_index`). Nothing written = no recording (folder removed,
  "nothing was recorded")
- Limits (`disk_check_locked`, before the start): projected size = bytes/s
  x length must fit in free - reserve (reserve = max(512 MiB, 2 % of the
  drive), <= 4 GiB); Unlimited needs >= 5 s; FAT32 (`diskutil::fs_type`
  vfat): a file must stay < 4 GiB (refused for a fixed length, a stop at the
  limit otherwise). While running: free < reserve -> stop + `halt` (what is
  still queued is dropped - it would eat the reserve), a write error / the
  FAT limit -> halt, no data for 5 s -> stop. A DRIVE SLOWER THAN THE DATA
  (this Pi's SD card: 8.4 MB/s by dd, 10.1 MB/s by the speed test; full band
  needs 24): the queue may hold `mem_budget()` = 40 % of MemAvailable (64 MB
  .. 1.5 GB) and the data is written after the end ("finishing", the page
  shows the rest); with the drive's speed known (`RECORDER_SPEEDTEST`: 128
  MB incompressible + sync_file_range + fdatasync, or the out rate of an
  earlier recording whose queue passed 64 MB) a length longer than budget /
  (need - speed) is refused with the reason, and a full queue STOPS the
  recording (reason "the disk is too slow") - a coherent recording with holes
  is worth less than a shorter one. Measured on the SD card: 10 s full band =
  10.0 s recorded, written out 32 s after the start, bearings exact
- Audio (`RECORDER_MODE:audio`, every mode, also DURING a playback - the way
  to get audio out of an IQ recording): `rec::feed_audio` per recorded VFO
  in the decimation pass (the stream its FM audio / decoders get: the
  beamformer output when it ran, else the listened-to channel; in fm_only
  mode the VFO counts as `single_wanted`), queue <= 3 s per VFO; the worker
  demodulates with its own `FMDemodulatorRobust` per VFO (the live audio's
  demodulator: filters, de-emphasis, levels) at 48 kHz; demod DIGITAL = the
  decoder's voice via the new record tap (`DigitalDecoder::set_record_tap`,
  `dig::set_record_voice_handler`; the vocoder runs as for Voice to Text;
  the worker sets the tap on every recorded VFO's decoder each second, so a
  decoder switched on later is tapped too) x6 to 48 kHz, put on the VFO's
  clock (silence where there is none). Squelch on and closed = silence, or
  left out (`RECORDER_AUDIO_SKIP`). Files `<name>_D<vfo>_<MHz>.wav|.opus`:
  WAV 16-bit mono 48 / 16 / 8 kHz (msresamp) with a LIST/INFO chunk (INAM
  VFO + MHz + demod, ICMT, IART station, ICRD, ISFT), sizes rewritten every
  5 s (a crash leaves a playable file); Ogg Opus 16-96 kbit/s (RFC 7845,
  VOIP <= 24 kbit/s, Vorbis comments TITLE / KRAKEN_VFO / _MHZ / _DEMOD /
  _LAT / _LON, a page ~ every second, granule trims the padding). A file
  about to pass 4 GiB continues as `_2`. Time limit checked every worker
  loop and input refused past it (was 1 s late: 10.45 s for 10)
- Commands (`RECORDER_*` + `GET_RECORDER` in `is_query_command`; the
  control handler records the applied settings itself; refusals ->
  `{"recorder":{"notice":..}}` to every page): settings (schema, after the
  web mapper) `RECORDER_DIR / _MODE iq|audio / _IQ full|band / _DURATION 0|
  10|30|60|120|300|600|1800|3600|7200|21600|43200|86400 / _BAND_VFO -1 (the
  first VFO heard, 🔊, else the first VFO)|id / _AUDIO_FORMAT wav|opus / _AUDIO_RATE 48000|16000|8000 /
  _AUDIO_KBPS 16|24|32|64|96 / _AUDIO_SKIP / _AUDIO_VFOS all|ids /
  _LOCATION` (dir / mode / iq / band VFO / format refused while recording);
  actions `RECORDER_START / _STOP / _LIST / _DELETE:<name> / _SPEEDTEST /
  _NAME:<name> / _NOTES:<text>` (name + notes cleared after a start).
  Status `{"recorder":{settings, durations, disk{dir_abs, free, total,
  reserve, fs, mount}, mounts[], need{bps, files, rate, channels, vfo,
  max_s, ram_s (-1 = keeps up / unknown), why, mem}, speed{mbps, t, live},
  test{running, pct, error}, cur (IQ: name, iq, state recording|finishing,
  elapsed_s = recorded, written_s, limit_s, bytes, queued, budget, in_bps,
  out_bps, bps; audio: vfos[{id, file, bytes, seconds, open, sq, mhz,
  demod}], dropped_s), last{kind, name, reason, error, bytes, seconds, t},
  error}}` on a change, every second while recording, every 3 s idle (the
  need follows the VFOs / mode / drive); list `{"recordings":{dir, dir_abs,
  items[{name, kind iq|audio, form folder|archive, bytes, mtime, writing,
  channels, rate, datatype, seconds, freq_hz, start, format, description |
  files[{name, bytes, seconds}] | error}]}}` on a change (1 Hz timer
  `rec::tick_messages`)
- HTTP (websocket_server.cpp, host check + token as /recordings):
  `GET /recording/<name>` (an IQ folder -> `<name>.sigmf`, an audio folder
  -> `<name>.tar`, an archive as it is; `rec::download` returns the parts -
  tar headers + file ranges - and `stream_parts` pumps them with tryEnd /
  onWritable; 409 while it is being recorded), `GET /recording/<name>/<file>`,
  `POST /recording_upload/<name>.sigmf` (Content-Length checked against free
  - reserve and FAT32; `rec::Upload` = its own writer thread, the socket
  `pause()`d above 16 MB queued and resumed via loop->defer below 4 MB; at
  the end fdatasync + `sigmf::open_recording` on the part file, else 422 and
  it is deleted; uploads to the SD card ran at its 10.7 MB/s)
- Playback (`playback.cpp`, the debug playback's machinery - sandbox,
  refusals, heimdall disconnected, nothing outward - with a second source):
  `PLAYBACK_OPEN:<name>[:<its VFOs 0|1>[:<loop 0|1>]]`, `PLAYBACK_PAUSE:0|1`,
  `PLAYBACK_SEEK:<s>` (applied by the receiver thread, packet-aligned),
  `PLAYBACK_LOOP:0|1`; refused while an IQ recording runs (and an IQ
  recording during any playback). `RecSource`: `sigmf::open_recording`
  (folder / archive / collection / meta; tar members found by `tar_index` -
  ustar + pax + GNU long names / base-256 sizes), every data file mmapped
  once; cu8 at 2.4 MHz copied as bytes (`fast`), anything else converted
  (`raw()`: every complex u/i 8-32 and f32/f64, le / be, iq_scale, foreign
  data > 0.95 or < 0.02 full scale brought to 0.5), interpolated x L like the
  test transmissions (`design_interp`, shared now) and put back at the VFO's
  offset (`krakensdr:band`, a double-precision phasor), + 0.6 LSB of dither.
  Header per packet from the capture segments: frequency (minus the band
  offset), per-antenna gain, phase state, noise flag = calibration (so the
  DoA holds as it did live), the change counter on a frequency change;
  `requested_freq_hz` follows (exact frequency). Session
  (`apply_rec_session`, 1.2 s after the start): coherent, the recording's
  `krakensdr:vfos` when asked, else the user's DECIMATORS again (every
  decoder was switched off first to drop the talkers), its
  `krakensdr:array_commands` (whitelisted prefixes), else its
  `spatial:element_geometry` as CUSTOM_POSITIONS, DOA:1 with >= 2 antennas,
  the station: `StationInfo::setPlaybackOverride` (new; resolve() returns it
  - a static one, or moving along `krakensdr:track` reported as a GPS fix
  with speed so mobile DF runs), cleared at Stop. Status `{"playback":
  {kind "rec"|"test", rec{name, channels, rate, datatype, format, dur_s,
  pos_s, paused, loop, ended, t_ms, start_ms, station, moving, vfos,
  description}}}`; the page interpolates the position between pushes
- UI (kraken_doa.html "📼 RECORDER" block + "📼 IQ & Audio Recorder" box
  after DoA Recording; element ids `iqr-*` - `rec-list` collided with the
  DoA Recording list, which then showed the IQ recordings; JS `rec*`): IQ /
  Audio switch, full band / VFO band (+ VFO), audio VFO ticks (All), format,
  leave out silence, lengths with their sizes (`doesn't fit` / `drive too
  slow` disabled; Unlimited says how long), name, notes, station position,
  the need line, Start / Stop, the running recording (progress, memory
  buffer, per-VFO open / closed / no squelch), the last result, Drive (drive
  list, folder, free bar, reserve, FAT32 note, ⏱ Test write speed), the list
  (📡 / 📦 / 🔊; ▶ Open, ⬇ download - streamed into a file with the File
  System Access API where the browser has it, else via a blob after asking
  above 1.5 GB -, audio ▶ listen / ⬇ per file, 🗑), ⬆ Upload (XHR progress,
  free-space check first), "Open with the recording's VFOs"; the playback's
  transport (position slider, pause, loop, recorded-at time, stop) at the
  top of the box; body class `pb-rec`: the banner / overlay "📼 PLAYBACK - not
  live" in purple with the name + position
- Shared: `include/utils/disk_util.hpp` (mkdirs, absolute, disk_space,
  fs_type, mounts_json - moved out of decoder_log.cpp, which uses it now)
- Tests (job scratch 46eef211, not in the repo; a TEST build on 5708x +
  fake_heimdall.py - 5-antenna UCA scene, CW at +100 kHz from 60 deg, FM
  tone at -300 kHz from 200 deg, calibration bursts, logged control port -,
  fake_gpsd.py, wsc.py, cdp.py headless Chromium): t1-t8 + t_ui (21 checks):
  full band 10 s -> 240 MB, 1 capture, hashes, bearings 60.0 / 200.0 from the
  recorded bytes (sigcheck.py, independent); live DoA 60 / 200 = playback 60
  / 200; VFO-band playback 200 on its VFO; pause / seek / loop off -> ended;
  refusals (FREQ, IQ during playback, a day of full band, traversal 404,
  folder-parent downloads 404); settings file byte-identical after Stop;
  audio WAV 16 kHz + Opus 24 / 32: the FM VFO a 1000 Hz tone, durations
  10.0 s, ffprobe tags; download tar = the folder byte for byte; upload ->
  archive playback 60 / 200; junk upload 422 + cleaned; SD card (RAM buffer,
  speed test, 60 s refused); unlimited band + Stop; retune during a band
  recording -> segments 433.70 MHz + calibration; bandwidth change stops;
  GPS track 29 points -> playback moves the station, rdf "gps" 15 m/s;
  shutdown mid-recording -> final metadata "kraken_doa shut down"; foreign
  sigmf-python recordings (3-antenna ci16 collection with spatial geometry
  only -> 130.0 deg; 1-channel cf32 at 240 kHz interpolated); debug playback
  regression (ais-four-ships 4/4 within 0.1 deg)

