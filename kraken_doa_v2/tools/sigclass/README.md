# Signal classifier: training data

Synthetic training data for the automatic signal classifier: clean
signals of every class, composed into captures like the receiver's
(2.4 MHz, 8-bit IQ, one antenna) with ground truth. Python 3 with numpy +
scipy only (what `install.sh` installs); on a machine whose first `python3`
is a conda without them, run `/usr/bin/python3`.

| File | What |
|---|---|
| `classes.json` | The class table: id, name, decoder plugin, click-to-tune demod / VFO width, occupied bandwidth range, burst / continuous, family; the RF plausibility table (per class `rf_hz` ranges, `generic`, top-level exclusive `bands`: the model carries it, the receiver masks by its tuned frequency). The single source of class ids. |
| `bank.py` | Builds the clean signal bank: per class many clips made by the plugins' own test-signal generators (`plugins/<id>/tools/*_gen.*`, run as separate programs) and `synth.py`, seeded random IDs, payloads, modes, speeds, timing. |
| `synth.py` | The classes no plugin generator makes: AM voice (25 / 8.33 kHz), SSB (USB / LSB), CW Morse, carriers, broadcast FM, generic OOK / FSK ISM devices; the unknown families (random FSK / GFSK / AFSK, PSK / QAM, OFDM, pulse trains, band-limited noise - kept away from the known systems' symbol rates, subcarrier spacings and pulse timings); physical-layer variants the plugin generators lack: a P25 control channel in C4FM and P25 in CQPSK / LSM (p25_gen.py's frame sync, NID and voice frames, random TSBKs), NXDN48 (nxdn_gen.py's filter and sync word, random payload symbols), VOR at measured depths; (2026-11-22) ISM FSK over bit times 10 us - 2 ms and tone spacings 10 - 200 kHz drawn independently (`ism_fsk2`), OOK with long / very short / shallow keying and ASK (`ism_ook2`), loud broadcast FM (`wfm_loud`). |
| `thresholds.py` | Per-class "sure" thresholds (`min_p`) for a trained model, from the synthetic val split and half of the own native captures. |
| `scenes.py` | The scene composer: 0.5 s (configurable) 2.4 MHz cu8 captures with 0-8 signals, channel and RTL-SDR impairments, truth as JSON lines, written in batches for the feature tool to consume. |

## Where the data goes

Never inside the repository. Work folder: `--work DIR`, else
`$KRAKEN_SIGCLASS_WORK`, else `~/.cache/kraken_sigclass`. The bank is
`WORK/bank/<class>/<clip>.cf32` (complex64, centred on 0 Hz, power 1 while
on air) + `.json` (rate, measured centre / 99 % bandwidth, on-air
intervals, duty, peak, variant tags, the generator commands) and
`WORK/bank/index.json`. About 43 GB for the default bank (1878 clips since
2026-11-22: + 160 ISM FSK, 100 ISM OOK, 16 FM clips, TETRA 24 -> 36 made
with `tetra_gen.py --traffic`; the 2.4 MHz classes are most of it). Captures go to `/dev/shm/kraken_sigclass/scenes`
unless `--out`.

## Build the bank

```bash
make -C kraken_doa_v2/plugins -j8          # the .cpp generators link plugins/lib/build/libkrakendig.a
python3 kraken_doa_v2/tools/sigclass/bank.py --work DIR --jobs 12        # ~15 min on 16 cores
python3 kraken_doa_v2/tools/sigclass/bank.py --work DIR --quick          # smoke test: 2 short clips a class
python3 kraken_doa_v2/tools/sigclass/bank.py --work DIR --classes dmr,lora
python3 kraken_doa_v2/tools/sigclass/bank.py --work DIR --stats          # clips, seconds, on-air, variants per class
```

Resumable: a clip whose `.json` exists is skipped, so after changing a
recipe move that class's folder away (the clips are seeded per class and
index - `--seed`, default 20261120 - and only reproduce with the recipe
that made them). bank.py sets one BLAS thread per worker (with the default
thread pools the workers hung in a futex wait on the 24 s LoRa clips).

## Make captures

```bash
python3 kraken_doa_v2/tools/sigclass/scenes.py --work DIR --count 100000 --batch 400 --jobs 12 \
        [--out /dev/shm/kraken_sigclass/scenes] [--max-gb 4] [--consume "featuretool {batch}"]
python3 kraken_doa_v2/tools/sigclass/scenes.py --work DIR --preview --out /tmp/prev    # 12 captures + summary.txt
python3 kraken_doa_v2/tools/sigclass/scenes.py --work DIR --single dmr --offset 250000 --snr 15 \
        --seconds 8 --no-fading --out /tmp/one        # one signal (decoder checks)
```

`DIR/batch_NNNNN/` holds `cap_IIIIIIIII.cu8` + `truth.jsonl`; a batch
appears under that name only when complete. The composer pauses while the
batches on disk exceed `--max-gb`; with `--consume` it runs the command on
each finished batch and deletes it afterwards. Capture `I` depends only on
`--seed` and `I` (`--first` starts elsewhere). One 0.5 s capture is 2.4 MB
and takes ~0.18 s of one core; 12 processes made 13-16 captures/s on a
16-core machine that was busy with other work.

Truth per signal: `class`, `clip`, `offset_hz` (99 % band centre),
`bw_hz` (SNR bandwidth), `lo_hz` / `hi_hz`, `snr_db` (while on air, noise
in `bw_hz`), `peak_snr_db`, `sinr_avg_db`, `on_air` (fraction of the
capture), `in_band`, `variant`, `ppm`, `fading`, `multipath`, `spur`. A
spur is a clean carrier labelled `carrier`. Impairments and their ranges
are listed in `scenes.py`'s docstring.

## Train the model

`train.py` (numpy + scipy; the MLP trainer is its own - Adam with decoupled
weight decay, dropout, label smoothing, class-weighted loss, seeded;
`train.py gradcheck` checks its gradients) and `eval_real.py`. Both run the
C++ core through `tools/build/sigclass_tool` (`make -C kraken_doa_v2
sigclass-tool`), so training features are the runtime's.

```bash
T=kraken_doa_v2/tools/sigclass; W=DIR
python3 $T/train.py gen --work $W --name V1 --count 24000 --seed 11    # captures -> features -> labelled batches (WORK/ds/V1/)
python3 $T/train.py gen --work $W --name VT --count 4000 --seed 2      # the test scenes (other seed)
python3 $T/train.py build --work $W --name V1 --out $W/ds/V1.npz        # one cache (retraining needs no regeneration)
python3 $T/train.py build --work $W --name VT --out $W/ds/VT.npz
python3 $T/train.py fit --work $W --data $W/ds/V1.npz,... --test $W/ds/VT.npz --none-weight 2.5 \
        --model kraken_doa_v2/sigclass.model --report $W/report.txt
python3 $T/eval_real.py --work $W [--library DIR] [--model M]          # the real recordings (table: WORK/eval_real.tsv + .txt)
```

The shipped model's data (layout v3, phase 4): X1 24 000 (seed 11), X2
16 000 (12), X3 3 000 of the wide / band-filling classes (13, `--classes`);
test XT 4 000 (2). The caches keep the symbol sequence apart (`Q`, int8 x
127) from the other features (`X`). `fit --conv-views 0,1 [--conv-k 32
--conv-l 24]` trains the learned front end (a Conv1D over the sequence;
`--conv-views none` = without - the shipped model, see the doc's Results);
`--drop-groups`, `--drop-edge-none` are ablations; `--exclude-view-classes`
(default: the 12.5 kHz channel classes out of view 1, which the analyzer's
second look in view 0 relies on). The 2026-11-22 data: Z1 24 000 (seed 11), Z2
16 000 (12), Z3 3 000 wide (13), Z4 16 000 (14), test ZT 4 000 (2), seven fits
(`--seed 1..7`), several fits averaged into one model (format 4):

```bash
python3 $T/train.py ensemble OUT.model FIT1.model FIT2.model ... --data $W/ds/Z1.npz,... --work $W
```

(per view the members' probabilities averaged; `min_confidence` refitted on
the val split). Then the per-class sure
thresholds (`min_p` in the metadata's classes - the service passes it
through on every track):

```bash
python3 $T/thresholds.py MODEL MODEL_OUT $W/ds/X1.npz,... EVAL.tsv --prec 0.95 --work $W --real-group confirmed
```

(the synthetic val split + half A of one `eval_real.py` group of the model's
table: `--real-group confirmed` = the decoder-confirmed public recordings,
crc32 of the entry id even - the global recipe of the shipped model since
2026-11-21, no own capture in the fit, so the own captures and public half B
are test data; `native` = half A of the own captures, the recipe before). `fit --no-merge` trains `merged_into` classes as their own (the
DVB-T2 / ISDB-T separation test). `KRAKEN_SIGCLASS_TOOL=path` makes
`eval_real.py` use another build of `sigclass_tool` (an older layout's, to
compare an older model on the same evaluation path).

- `gen` runs `scenes.py` with `--consume "train.py ingest {batch}"`: each
  batch of captures goes through `sigclass_tool features`, its detections
  are matched to the truth (rules in `train.py`'s docstring: one truth -
  one detection labelled; fragments of a signal keep its class; a detection
  covering several signals only when one dominates by 6 dB, else dropped;
  detections near no signal are `none`, but one next to a strong signal -
  >= 20 dB, within max(2 x its width, 25 kHz), >= 15 dB weaker - or at its
  IQ image is dropped: `near_strong`, also applied when a cache is loaded),
  and the captures are deleted.
- Splits by bank clip (`split_of`: per class every 10th clip test, the next
  val, the rest train - payloads, IDs and modes of the test clips are never
  trained on); the test set also comes from other scenes (seed 2).
- Per view (24 k / 96 k / 480 k / 2.4 MHz) one MLP over the classes with
  >= `--min-examples` training examples in that view; a temperature per view
  fitted on val; `min_confidence` = the lowest threshold at which >= 95 % of
  the shown (not `none`) val labels are right.

Model metadata (`meta` in `sigclass.model`): `classes` = the `classes`
array of `classes.json` verbatim (`none` has `hidden: true`: the service
never publishes such a track), `min_confidence`, `trained`, `views` (each
view's class ids, train / val sizes, temperature), `dataset`, `trainer`,
`layout_hash`.

`eval_real.py` runs the model over a local library of public and own
recordings (`--library`, `$KRAKEN_IQ_LIBRARY`, else `~/krakensdr_iq_library`;
read-only; the library's `iqlib` reads one antenna of a multi-channel entry
by its `channel_layout`) the way the receiver sees them, track level (the
tracks after the last snapshot of a 4 s window), one table row per expected
signal (the library's `expect` rows). Its docstring has every rule; in short
(2026-11-21 rework, after the per-recording failure analysis found that 44 of
85 failing real rows were evaluation / truth faults):

- **Only while on air.** A row with `on_air: "intermittent"` is scored only
  in a window that holds >= 2 s of its `on_air_s` intervals (half the
  window's 8 snapshots: the track's evidence then rests mostly on looks with
  the signal present) or one of its transmissions whole (a burst or packet is
  seen complete in one snapshot); without intervals it is "not known on air"
  and stays out of the score. Long captures with intermittent rows get
  several windows, chosen greedily from the intervals (each adds the most
  rows not yet on air in an earlier one, `--max-windows`, default 12); every
  row is scored once, in the window where it is on air longest; continuous
  rows in the entry's usual window (the busiest stretch, after an opening
  run of exact zeros).
- **Matching.** Among the tracks overlapping a row's band, a band-wide row
  (>= 2 MHz) takes only tracks >= 25 % of its width (a spur inside is not its
  track); a narrower one prefers tracks <= max(4 x bw, 30 kHz); of those only
  the tracks within 10 dB of the strongest compete (a track's strength = the
  highest in-band SNR of the detections in its band, any snapshot: a row
  describes the dominant signal in its band, a ~1 dB receiver hump must not
  beat the VOR's own track); then the best band overlap (intersection /
  union), then hits (most hits alone picked a band-edge spur over the right
  track). The table's `iou_pick` column holds the track the overlap-first
  rule would take: on the whole library 4 rows change, none for the worse
  (the VOR wrong -> right; two radiosondes wrong -> wrong; a CW row right ->
  right). Right = the row's class or any of its `classes` (one channel
  alternating two systems).
- **Feed.** The feed may remove what the recording device added (its band
  edge, its DC line, its squelch gaps), never clean the signal. Own captures:
  the bytes as they are. IQ >= 2 MS/s: resampled. IQ below 2 MS/s: gaps
  (exact-zero runs >= 1 ms) get noise at the quiet level; the recording's DC
  line (classes without a carrier) is attenuated to the floor beside it - no
  noise injected; resampled, placed +300 kHz off the centre, then
  band-limited: each row's signal band (half = min(row bw, the class's widest
  occupied bw) / 2) plus a margin (crop = max(1.8 x half, half + 2 kHz)),
  clipped to the recording's passband (bins whose quiet floor is within 10 dB
  of the floor around the signal), is kept, the rest of the 2.4 MHz band is
  white noise at the passband floor just beside the signal. Before, a
  recording's own floor ~1 dB over a white fill was detected as a 70-100 kHz
  "signal" (fixed 9 of 9 such rows in the analysis), and an IF-filtered
  recording's passband noise (APRS, VDL2) became a "noise-like signal"
  because the fill took the stopband level. Discriminator audio: FM
  re-modulated (a weaker test, apart). 8 bits at a realistic level, >= 0.8
  LSB of receiver noise.
- **Undetermined truth**: a row with `truth: "undetermined"` (the library
  could not establish its class from the recording - 14 ISM rows since
  2026-11-22: no sample rate, clipped for the whole burst, too short, no
  keying visible) is in the group `undetermined`: listed, never scored (an
  entry with only such rows is not fed at all). `--index FILE` evaluates
  against another index.json than the library's (a staged one).
- **Groups, per row** from what the library states: native (own captures in
  the receiver's format); open (no class for it, or `unknown`); disc_audio;
  confirmed (rtl_433's corpus, or `verified` reports a positive decoder count
  and the row's own note does not call it undecoded / unverified /
  undetermined); probe (no decoder run); unconfirmed (the rest). The old
  whole-entry text match filed a decoder-confirmed DMR entry as unconfirmed
  because its second carrier's note said "0 frames".
- **RF mask** (`--rf-mask on`, the default): the entry's `center_hz` (less
  the placement shift) goes to `classify --rf`.
- **Output**: the table (scored or not, with the reason; window, on-air
  seconds, crop, fill level), and TABLE.txt: per group and class (exact,
  family, shown), the trust table per group (sure right / sure wrong /
  unsure right / unsure wrong / hidden, sure share; sure = p >= the class's
  `min_p`, else `min_confidence`), and the false labels - every drawn track
  (p >= 0.5, not `none`) overlapping no row of its entry, sure / unsure, with
  single-service recordings, busy sites (>= 5 rows: their unidentified
  channels are mostly real transmitters) and captures the library marks as
  intermodulation-dominated (real RF, not transmitters) apart.
- `--selfcheck [--bank DIR]`: the feed path on synthetic captures with known
  answers (a `scenes.py --single` DMR and POCSAG as a native 2.4 MS/s file
  and as 48 kHz recordings - plain, IF-filtered with a raised passband floor,
  opening with 3 s of exact zeros -, a silent file, a noise-only file); exit
  1 on a failure. The old feed failed the two IF-filtered cases (unknown
  noise); the new one passes all 10.

## Results (2026-11-22: corrected ISM truth, retrain - the weights not replaced)

Full tables in the doc ("Results (2026-11-22)"). On the corrected library,
track level, shipped -> the best new ensemble (5 fits averaged, chosen by
synthetic val + public half A): public confirmed 193 -> 262-263 of 336, ISM
124 -> 193-195 of 240, FSK twins 26 -> 71 of 94, non-ISM 69 -> 68-69, own
captures 112 -> 108-110 of 122 (P25 channels of two sites, a DVB-T band-edge
capture), playback 27 / 27 (DVB-T as ATSC 3.0). By the acceptance stated
beforehand the new weights do not ship; `sigclass.model` stays, with the new
detector (own 112, confirmed 196, fewer false labels). Seven fits a
generation: single-fit spreads in the doc.

## Results (2026-11-21: the fair evaluation, the core fixes, global thresholds)

Same weights (layout v3) throughout. Columns: (1) the old evaluation + the
shipped core (old library), (2) the new evaluation + the shipped core, (3)
the new evaluation + the core of 2026-11-21 (tracker / view-0 alternative /
image rule). Rows scored / top-1 / family; whole library (506 entries, 364
rows), 390-400 s with 5 jobs.

| set | (1) | (2) | (3) |
|---|---|---|---|
| own captures, all | 110 / 68 / 69 | 123 / 103 / 104 | 123 / 113 / 112 |
| own, single-service | 41 / 32 / 30 | 36 / 34 / 33 | 36 / 34 / 33 |
| own, busy trunked sites | 57 / 30 / 33 | 83 / 67 / 69 | 83 / 77 / 77 |
| own, IM-dominated capture | 12 / 6 / 6 | 4 / 2 / 2 | 4 / 2 / 2 |
| public, decoder-confirmed | 357 / 177 / 227 | 343 / 187 / 228 | 343 / 188 / 229 |
| ... without ISM OOK / FSK | 110 / 59 / 78 | 96 / 67 / 77 | 96 / 69 / 79 |
| probe | 30 / 7 / 15 | 35 / 13 / 22 | 35 / 14 / 22 |
| unconfirmed | 27 / 3 / 4 | 31 / 6 / 7 | 31 / 6 / 7 |
| discriminator audio | 29 / 11 / 16 | 29 / 13 / 18 | 29 / 13 / 18 |
| open set (right) | 42 of 51 | 43 of 53 | 42 of 53 |

Confirmed per class, (1) -> (3): DMR 3/5 -> 5/6, P25 9/19 -> 12/15 (the
library moved rows to P25 Phase 2: 1/2 -> 2/5), NXDN 2/5 -> 5/5, YSF 1/3 ->
3/3, radiosonde 3/14 -> 6/14, AIS 0/2 -> 2/2, ISM FSK 21/98 -> 22/98, ISM
OOK 97/149 -> 97/149. Own-capture rows still wrong in (3), 10: four DMR
channels and two MPT1327 control channels swallowed by a wide max-hold burst
track (416.5 MHz, a 1.4 / 2.0 MHz "LoRa" / "carrier" track), a P25 voice
channel merged with the receiver's ~251 kHz-comb hump, a weak DMR channel
merged with a neighbour, LoRa below the noise, a weak FM station.

Thresholds (the per-class `min_p`; `min_confidence` 0.82 in all): T0 =
shipped (synthetic val + own half A, old evaluation), T1 = that recipe
refitted on the new evaluation (identical to T0), T2 = the global recipe
(synthetic val + public confirmed half A, crc32 even; no own capture): T0's
min_p plus `radar` 0.86, `ism_ook` 1.01 (never sure), `unk_fsk` 0.99. Track
level, new evaluation + new core, sure right / sure wrong (precision) /
unsure right / unsure wrong / hidden / sure share:

| set | T0 = T1 | T2 (shipped) |
|---|---|---|
| own captures (test data for T2) | 104 / 0 (1.00) / 4 / 7 / 7 / 0.85 | 104 / 0 (1.00) / 4 / 7 / 7 / 0.85 |
| public half B (not fitted) | 44 / 5 (0.90) / 38 / 34 / 53 / 0.28 | 35 / 1 (0.97) / 47 / 38 / 53 / 0.21 |
| public half B without ISM | 30 / 0 (1.00) / 3 / 5 / 8 / 0.65 | the same |

What T2 gives up: ISM OOK labels are never sure (a public half-A ISM OOK
label was confidently wrong at 1.00); public sure share 0.28 -> 0.21.
False labels (drawn at no row; (3), sure / unsure): single-service own
captures 0 / 2, public single-service 1 / 15, busy sites 22 / 33 (mostly
real unidentified channels), the IM-dominated capture 4 / 0; the same under
T0 and T2.

## Results (2026-11-20, phase 5: the phase 4 weights + the RF mask, model format 3)

The RF plausibility mask (the model carries `classes.json`'s table), the
refitted per-class thresholds (false labels on own captures included), the
tracker's evidence over ~10 looks. Track level, 6d51b53 -> now: own native
captures 65 -> 68 of 110 (the mask: FM, the ILS-on-a-spur false label);
library confirmed 44 -> 50 % top-1; sure labels right 0.84 -> 1.00 (own
half B, 56 % of signals sure) and 0.71 -> 0.91 (library, 24 % sure); no
sure false label on a single-service own capture; the empty band shows
nothing. Debug playback strict: see the doc.

## Results (2026-11-20, phase 4: layout v3, `sigclass.model` 3.8 MB)

Full tables, the diagnosis and the earlier rounds: `docs/dev/signal-classifier.md`
"Results". The shipped model: X1-X3 (43 000 captures), no learned front
end (it was no better on real data; the code is in and off), per-class
`min_p` thresholds. Track level, before = the 6d51b53 model:

- debug playback, strict (the entry's own class): 27 -> 26 of 28 (DMR and a
  one-hit FLARM piece wrong; DVB-T, radiosonde, wildlife fixed)
- own native captures: 65 -> 67 of 110; a SURE label right 0.84 -> 1.00 on
  the half not used for thresholds (54 % of the signals sure)
- public library, decoder-confirmed: 44 -> 48 % top-1; sure labels right
  0.71 -> 0.90 (22 % sure)
- synthetic test XT: top-1 0.855, family 0.874
- symbol rate found in 65 % of the real 4-FSK / TETRA / pager / sonde
  detections

## Known limits

- dmr / dstar / nxdn / p25 / tetra generators take `--seed` (the bank seeds
  every clip); sfn_gen and dab_gen have none (dab_gen's audio is a tone per
  service), analogtv has one test card, tetra_gen's payload is the empty
  SCH/F every frame without `--traffic` (which the bank uses since 2026-11-22).
- tetra_gen.py's output does not decode for 18 of the 64 colour codes
  (2, 3, 5, 8, 10, 17-20, 27, 28, 34, 38-40, 46, 52, 54): the bank uses
  the others.
- NXDN48, the P25 control channel and P25 CQPSK / LSM come from synth.py
  (physical layer from the plugin generators' framing pieces + random
  payload; NXDN48's frame periodicity is weaker than a real one's);
  radiosondes are RS41 and M20 only (no generator for the other types).
- The learned front end over the symbol sequence (`fit --conv-views`)
  learns frame-synchronous filters (they fire once per DMR / P25 / NXDN /
  TETRA frame) but did not improve the real tables yet: the symbol rate is
  wrong in a third of the real 4-FSK detections, and the sequence then is
  noise.
- Real-data scores vary a lot between training sets of the same recipe (ISM
  OOK 29-95 of 149).
- DVB-T2 and ISDB-T are trained as DVB-T (`merged_into` in classes.json):
  neither the v1 log lag grid nor the v2 linear one separates them well
  enough (`fit --no-merge`: ISDB-T 0.5, a third of it DVB-T).
- A detection of a wide class in a view too narrow for it (the class's
  minimum occupied bandwidth > 2 x the view's widest) is trained as `none`.
- No sample-rate error on clips at >= 1.2 MHz (wide signals); at 48 kHz it
  is whole samples (~38 ppm steps).
