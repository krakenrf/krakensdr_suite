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
| `synth.py` | The classes no plugin generator makes: AM voice (25 / 8.33 kHz), SSB (USB / LSB), CW Morse, carriers, broadcast FM, generic OOK / FSK ISM devices; the unknown families (random FSK / GFSK / AFSK, PSK / QAM, OFDM, pulse trains, band-limited noise - kept away from the known systems' symbol rates, subcarrier spacings and pulse timings); physical-layer variants the plugin generators lack: a P25 control channel in C4FM and P25 in CQPSK / LSM (p25_gen.py's frame sync, NID and voice frames, random TSBKs), NXDN48 (nxdn_gen.py's filter and sync word, random payload symbols), VOR at measured depths. |
| `thresholds.py` | Per-class "sure" thresholds (`min_p`) for a trained model, from the synthetic val split and half of the own native captures. |
| `scenes.py` | The scene composer: 0.5 s (configurable) 2.4 MHz cu8 captures with 0-8 signals, channel and RTL-SDR impairments, truth as JSON lines, written in batches for the feature tool to consume. |

## Where the data goes

Never inside the repository. Work folder: `--work DIR`, else
`$KRAKEN_SIGCLASS_WORK`, else `~/.cache/kraken_sigclass`. The bank is
`WORK/bank/<class>/<clip>.cf32` (complex64, centred on 0 Hz, power 1 while
on air) + `.json` (rate, measured centre / 99 % bandwidth, on-air
intervals, duty, peak, variant tags, the generator commands) and
`WORK/bank/index.json`. About 39 GB for the default bank (1582 clips, 4.6 h
of signal; the 2.4 MHz classes are most of it). Captures go to `/dev/shm/kraken_sigclass/scenes`
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
`--drop-groups`, `--drop-edge-none` are ablations. Then the per-class sure
thresholds (`min_p` in the metadata's classes - the service passes it
through on every track):

```bash
python3 $T/thresholds.py MODEL MODEL_OUT $W/ds/X1.npz,... NATIVE_EVAL.tsv --prec 0.95 --work $W
```

(the synthetic val split + half A of the own native captures, crc32 of the
capture id even, from an `eval_real.py` table of the model; half B and the
public library stay test data). `fit --no-merge` trains `merged_into` classes as their own (the
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
read-only) the way the receiver sees them: the receiver's own 2.4 MS/s
bytes as they are; narrower IQ resampled to 2.4 MHz, placed 300 kHz off
the centre and the band filled with noise at the recording's own density
(pedestal measured: median 1.0 dB, > 3 dB in 3 of 377); discriminator
audio FM re-modulated (weaker test); then `sigclass_tool classify --track`
and the tracks of the last snapshot (track level, not per detection)
compared with the entry's expected signals. Recording artefacts that a
receiver tuned elsewhere would not see are removed first: a squelch-gated
file's exact-zero runs (>= 1 ms) get noise at its own quiet level; for
classes without a carrier the recording receiver's DC spike (a line within
+-100 Hz, >= 10 dB over its surroundings) is replaced by noise; the 4 s
window is the busiest of the first 120 s when that is clearly busier than
the start (files that open with silence); a recording with more SNR than 8
bits hold keeps >= 0.8 LSB of receiver noise (below that the noise vanished
with the signal: a whole-band "burst"). Scored exact (the class) and by
family (`coarse_family`: classes.json's modulation grouped into fsk, psk,
ofdm, vsb, pulse, am, fm, css, noise, none - unknown FSK for a DMR signal
is family-right). With `--rf-mask on` (the default) the entry's
`center_hz` (less the placement shift) goes to `classify --rf`: the model's
RF mask, as the receiver applies it (`off` to compare; the library knows
the centre of 201 of 338 confirmed entries and of every own capture).
False labels: every drawn track (p >= 0.5, not `none`) that matches no
identified signal of an own capture goes into `false_tracks` (sure / unsure
by the model's thresholds) and the summary.

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
  SCH/F every frame.
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
