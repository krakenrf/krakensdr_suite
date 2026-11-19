# Signal classifier: training data

Synthetic training data for the automatic signal classifier: clean
signals of every class, composed into captures like the receiver's
(2.4 MHz, 8-bit IQ, one antenna) with ground truth. Python 3 with numpy +
scipy only (what `install.sh` installs); on a machine whose first `python3`
is a conda without them, run `/usr/bin/python3`.

| File | What |
|---|---|
| `classes.json` | The class table: id, name, decoder plugin, click-to-tune demod / VFO width, occupied bandwidth range, burst / continuous, family. The single source of class ids. |
| `bank.py` | Builds the clean signal bank: per class many clips made by the plugins' own test-signal generators (`plugins/<id>/tools/*_gen.*`, run as separate programs) and `synth.py`, seeded random IDs, payloads, modes, speeds, timing. |
| `synth.py` | The classes no plugin generator makes: AM voice (25 / 8.33 kHz), SSB (USB / LSB), CW Morse, carriers; the unknown families (random FSK / GFSK / AFSK, PSK / QAM, OFDM, pulse trains, band-limited noise - kept away from the known systems' symbol rates, subcarrier spacings and pulse timings). |
| `scenes.py` | The scene composer: 0.5 s (configurable) 2.4 MHz cu8 captures with 0-8 signals, channel and RTL-SDR impairments, truth as JSON lines, written in batches for the feature tool to consume. |

## Where the data goes

Never inside the repository. Work folder: `--work DIR`, else
`$KRAKEN_SIGCLASS_WORK`, else `~/.cache/kraken_sigclass`. The bank is
`WORK/bank/<class>/<clip>.cf32` (complex64, centred on 0 Hz, power 1 while
on air) + `.json` (rate, measured centre / 99 % bandwidth, on-air
intervals, duty, peak, variant tags, the generator commands) and
`WORK/bank/index.json`. About 37 GB for the default bank (1498 clips, 4.2 h
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
recipe delete that class's folder (the clips are seeded per class and
index - `--seed`, default 20261120 - and only reproduce with the recipe
that made them).

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
python3 $T/train.py gen --work $W --name A2 --count 24000 --seed 1     # captures -> features -> labelled batches (WORK/ds/A2/)
python3 $T/train.py gen --work $W --name B --count 4000 --seed 2       # the test scenes (other seed)
python3 $T/train.py build --work $W --name A2 --out $W/ds/A2.npz        # one cache (retraining needs no regeneration)
python3 $T/train.py build --work $W --name B --out $W/ds/B.npz
python3 $T/train.py fit --work $W --data $W/ds/A2.npz --test $W/ds/B.npz --model kraken_doa_v2/sigclass.model --report $W/report.txt
python3 $T/eval_real.py --work $W [--library DIR] [--model M]          # the real recordings (table: WORK/eval_real.tsv + .txt)
```

- `gen` runs `scenes.py` with `--consume "train.py ingest {batch}"`: each
  batch of captures goes through `sigclass_tool features`, its detections
  are matched to the truth (rules in `train.py`'s docstring: one truth -
  one detection labelled; fragments of a signal keep its class; a detection
  covering several signals only when one dominates by 6 dB, else dropped;
  detections near no signal are `none`), and the captures are deleted.
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
(pedestal measured: median 1.0 dB, > 3 dB in 2 of 377); discriminator
audio FM re-modulated (weaker test); then `sigclass_tool classify --track`.

## Results (2026-11-20, model 804-192-96-C, `sigclass.model` 2.84 MB)

Data: 46 000 training captures (A2 24 k + C 12 k + D 4 k + D2 2 k of the
wide classes + E 4 k with sub-LSB receiver noise; 0.32 M detections, 0.29 M
labelled incl. `none`, 0.22 M of train clips), test = 4 000 captures of another
seed (B2), test clips only.
Views (classes): 24 kHz 32, 96 kHz 18, 480 kHz 15, 2.4 MHz 19.
`min_confidence` 0.82 (95 % of the shown val labels right, 78 % shown).

- **Synthetic test** (2 740 detections): top-1 0.867, top-3 0.959, macro
  recall 0.859 / precision 0.803 over 49 classes; per view top-1 0.870 /
  0.873 / 0.887 / 0.771. Weakest: radar 0.53 (<-> ssr1030), adsb 0.56,
  unk_fsk 0.41, unk_psk / unk_noise ~0.5, nfm_voice 0.63, ism_fsk 0.77,
  flex 0.78 (-> carrier), none 0.88. Accuracy vs SNR: 0.50 (< 0 dB), 0.74
  (5-10), 0.87-0.93 (>= 10). Learning curve (per-view training cap): 10 k
  0.814, 30 k 0.842, all 0.867 - more data still helps. Size: 128-64 0.853
  (1.84 MB), 192-96 0.867, 256-128 0.865 (3.9 MB). `none` shown as a signal:
  5 % of non-signal detections, 0.17 a capture (none weight 2.5: top-1
  0.871, none 0.90 - an option, not the shipped model)
- **Detection ceiling**: 0.82-0.90 of the narrow truths at any SNR, 1.00
  for the band-filling ones; nearly all misses at high SNR are signals
  placed across the band edge (the detector's 3 % edge guards)
- **Own captures, the receiver's native bytes** (41 expected signals):
  top-1 26 (63 %); DVB-T 18 / 19, ADS-B, DME, VOR's crystal-harmonic
  carrier, two of three FM stations right; LoRa, LTE, NR, SSR 1030 (real
  ones: `none` - not shown), VOR (unknown PSK), ISM 433 (unknown OFDM / ISM
  FSK) wrong; the empty band (1290 MHz): no shown label
- **Library, decoder-confirmed** (357 signals, 247 of them rtl_433 ISM test
  files): top-1 38 %, top-3 54 %; ISM OOK 65 / 149, ISM FSK 27 / 98, FM
  broadcast 9 / 13, DAB 8 / 9, VOR 3 / 3, AIS 2 / 2, P25 6 / 19, radiosonde
  3 / 14 (DFM / iMet / LMS6 / M10: no such sonde in the bank). Spectrum-
  probe-only 20 %, unconfirmed 4 %, disc_audio 17 %; open set (no class):
  80 % labelled unknown / none / below `min_confidence`
- **Debug playback through the TEST pipeline**: see
  `docs/dev/signal-classifier.md` "Results"

## Known limits

- Several generators use a fixed random seed for their payload (dmr,
  dstar, nxdn, p25 phase 1, tetra, sfn_gen, dab_gen, analogtv's fixed test
  card): the variety there comes from IDs, colour codes, timing, modes and
  offsets only.
- tetra_gen.py's output does not decode for 18 of the 64 colour codes
  (2, 3, 5, 8, 10, 17-20, 27, 28, 34, 38-40, 46, 52, 54): the bank uses
  the others.
- NXDN is NXDN96 only (nxdn_gen.py has no 4800 bit/s NXDN48); radiosondes
  are RS41 and M20 only.
- DVB-T2 and ISDB-T are trained as DVB-T (`merged_into` in classes.json):
  the v1 features' log lag grid cannot tell their symbol lengths apart.
- A detection of a wide class in a view too narrow for it (the class's
  minimum occupied bandwidth > 2 x the view's widest) is trained as `none`.
- No sample-rate error on clips at >= 1.2 MHz (wide signals); at 48 kHz it
  is whole samples (~38 ppm steps).
