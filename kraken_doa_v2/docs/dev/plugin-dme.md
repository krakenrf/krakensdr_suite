# DME / TACAN plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**DME / TACAN (`plugins/dme/`, README.md there):**
- 2.4 MHz input (`min_vfo_rate` 600 kHz, manual only, talkers). From
  `Host::rf_hz()` every integer MHz of 962..1213 within +-0.8 MHz of the
  VFO centre is a channel (no RF: the centre, unnamed). FFT channelizer:
  4096 points, overlap-save (OVL 1024, hop 3072), per channel the bins
  around its centre (raised cosine flat +-350 kHz, zero +-550 kHz) -> 2048
  IFFT = 1.2 MHz, the block's shift phase undone (continuous carrier
  phase). Channel sample K = input sample in0_ + 2K; `abs_in_` never resets
  (Host::time()). (FFTW_MEASURE planned for 32 s on the Pi; pffft since
  2026-10-17 has no planning)
- Pulses: noise = median magnitude (every 7th sample, ~50 ms, smoothed),
  threshold `min_snr` (10 dB, peak power over median^2 / ln 2), local max
  +-3 samples, half-amplitude width 2.4-5.6 us (SSR 0.8 us pulses fail),
  leading edge at 50 % interpolated, rise 10-90 %, phase steps for the
  carrier. Pairs: 12 / 30 / 36 us +-1, <= 4 dB apart, nearest spacing wins;
  `role_of(mhz, kind)`: 12 us on 962-1024 / 1151-1213 = X replies, 30 us on
  1025-1150 = Y replies, 12 / 36 us on 1025-1150 = X / Y interrogations of
  channel mhz - 1024 (`nav::dme_by_reply` / `dme_by_interrogation`)
- Ident: a reply pair with one 1/1350 s (+-5 us, refined while keyed)
  earlier is "periodic"; per 10 ms tick key = >= 6 pairs and most periodic,
  3-tick majority -> `nav::MorseDecoder` (unit 0.07-0.2 s, end gap 1.2 s);
  reported when clean (no '?') with timing fit >= 0.6
- TACAN: reference bursts = runs of >= 5 equally spaced 12 us pairs every
  20-36 us (>= 9 = north) or >= 9 single pulses at 12 / 15 / 30 us (Y: 30 =
  north, 15 = auxiliary; X pulses every 12 us: >= 18 = north) - the
  sources disagree on the X spacing, both are taken; TACAN = 10+ north
  bursts 1/15 s apart in 2 s. Reply pulses wait 0.1 s (`tpulses`), then
  those outside bursts with a turn (north burst before and after, 1/15 s
  +-5 %) go into a 5-term least-squares fit (c0, 15 Hz, 135 Hz; decays over
  ~3 s): bearing = 90° + the 15 Hz maximum's phase (north burst = maximum
  east, clockwise), refined by the 135 Hz candidate nearest it
- Output: table row per channel AND pair kind (`row_key` mhz/kind:
  replies and interrogations on one frequency are separate rows;
  interrogations "about N aircraft" = clusters of pair levels > 2 dB
  apart), facts for the busiest reply channel (+ "Channels heard",
  "Interrogations"), events (heard, ident, TACAN, quiet), raw = idents.
  `host.valid()` every 0.25 s with >= 3 pairs (the Digital squelch holds
  1.5 s). DoA: clean pairs (no pulse between, none 15 us before / after),
  >= 15 dB, token bucket `doa_rate` (100/s) -> Talker packet id = channel
  ("40X"), label "IAA · 1001 MHz", start - 4 us .. second pulse + 8 us (>=
  2 whole 16-sample chunks), freq_hz = the channel's offset, avg_s 10
- CPU (Pi 5): 3.3 % of a core per channel (scratchpad bench); the offline
  host's double sin/cos NCO doubles the figure it prints
- Tests (scratchpad dme/): the live 40X recordings (90 s / 480 kHz in the
  AI session, 20 s / 2.4 MHz), gen_dme.py synthetic A (18Y + 81X
  interrogations + Y TACAN 237°), B (weak 18X + X TACAN 123.4°), C (SSR
  on 1030 MHz), D (no RF), N (noise); end to end feed.py + t_dme_e2e.py
  (beacon 69.4° vs whole VFO 198°), t_dme_sq.py (Digital squelch: VFO DoA
  69-70°, open 20/20), t_dme_ui.py (decoder tab, DF panel)
- Generator in the repo (2026-10-16): `tools/dme_gen.py` (streamed 2.4 MHz
  scene: Gaussian 3.5 us pulse pairs X 12 / Y 30 us, interrogations 12 /
  36 us, squitter with a 60 us dead time, Morse idents at 1350 pairs/s,
  TACAN reference bursts + 15 / 135 Hz modulation) + `tools/scene_two.json`
  = playback `dme-two-beacons` (63Y TACAN 237° ident TKN at 60°, 64X ident
  KDF at 200°, two aircraft on 126X): offline both idents, TACAN 236.9°,
  bearings 60.0 / 200.0; end to end 60.0 / 199.8

