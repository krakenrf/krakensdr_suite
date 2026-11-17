# Deep-TEMPEST in kraken_doa: an integration plan (not yet implemented)

This is a **design plan**, not shipped code. It describes how neural-network
raster reconstruction ("Deep-TEMPEST") could be added to the suite on top of
the classical `tempest` plugin, and - just as important - the licensing and
platform walls that decide *how* it may be done here. Nothing in this file is
built yet; the `tempest` plugin's classical reconstruction (*tempest*) is the
starting point.

## What Deep-TEMPEST is

Deep-TEMPEST (Fernández, Martínez, Varela, Musé, Larroca, Universidad de la
República, arXiv:2407.09717, 2024) recasts screen-leakage recovery as an
**inverse problem**: a convolutional network maps the SDR-captured emanation,
after it has been aligned into a raster image, to an estimate of the image
that was actually on the screen. Reported result: a reduction of text
Character Error Rate (CER) of "over 60 percentage points" versus classical
reconstruction - i.e. unreadable → largely readable text.

Key facts that shape our plan (from the paper and its repository; verify exact
hyper-parameters against the source before building):

- **Network:** a **DRUNet**-style denoiser/restorer (a bias-free residual
  U-Net, Zhang et al. "Plug-and-Play Image Restoration with Deep Denoiser
  Prior"). It is an image→image model, not a sequence model.
- **Input:** the complex capture is first synchronised and cropped to one
  frame's raster (exactly the classical pipeline's job), then presented to the
  net as a **real-valued image** - the paper uses the complex samples' real
  and imaginary parts (and/or magnitude) laid out on the raster grid as input
  channels. The network output is a single-channel (grayscale) clean image.
- **Training data:** paired `(emanation raster, ground-truth screen image)`.
  The authors released a dataset of simulated pairs plus 1000+ real captures.
  The ground truth is the framebuffer that was being displayed; the input is
  the time-aligned emanation of that same frame.
- **Metric / loss:** CER is the headline metric for text; training uses an
  image-restoration loss (pixel L1/L2, the paper's repo has the exact recipe).
- **Stack:** built on **gr-tempest** inside **GNU Radio**, with **PyTorch**,
  Python 3.10, in a conda environment.

## The two walls

**1. Licensing (hard).** `kraken_doa_v2/` - including `plugins/` and
`plugins/lib/` - is under the KrakenSDR Source Available Licence. The suite
rule (`../../CLAUDE.md`) forbids GPL/AGPL/LGPL code in it or linked into it,
**and** forbids code *translated* from GPL projects. Deep-TEMPEST's code and
gr-tempest are GPL-3. Therefore:

- We may **not** copy, link, or port Deep-TEMPEST or gr-tempest code, and may
  **not** translate their GNU Radio blocks into our C++.
- We may read the **paper** (facts, architecture description, math) and use the
  released **dataset** as a *reference measurement* for our own clean-room
  work, exactly as the suite already treats rtl_433 / dump1090 / OP25 as
  documentation of facts only.
- Any model *runner* we embed must itself be permissive. Candidates:
  **ggml** (MIT), **tract** (MIT/Apache-2.0), **ONNX Runtime** (MIT). The
  weights are data, not code; a user-supplied or self-trained ONNX/gguf file
  carries no GPL obligation onto our binary. A plugin that embeds such a
  runner would, like `ism`/`flarm`, still live under our source-available
  licence for *our* code, with the runner as a permissively-licensed
  dependency recorded in `THIRD_PARTY_NOTICES.md` and
  `scripts/collect-licenses.sh`.
- The clean-room path: implement the DRUNet forward pass **from the published
  architecture** (it is a standard residual U-Net - conv/ReLU/strided
  down, transpose-conv up, skip connections), load weights we trained
  ourselves (or that the user provides), and never consult the GPL code.

**2. Platform (hard).** The real-time target is a Raspberry Pi 4/5 next to the
receiver, **no network**, and every decoder is an out-of-process plugin that
must not stall the pipeline. PyTorch + GNU Radio are out of the question on
that path. Consequences:

- No Python/PyTorch/conda in the live plugin. Inference, if on-device, is a
  single statically-linked C++ forward pass (ggml/tract) or nothing.
- A full-resolution DRUNet forward pass per frame will **not** keep up at
  frame rate on a Pi. Deep reconstruction is therefore **not** a live,
  every-frame mode; it is an **on-demand enhancement** of an accumulated,
  already-aligned still (the screen content is near-static - that is what
  makes the attack work).

## How it attaches to this codebase

The classical `tempest` plugin already does everything up to the network's
input: AM-magnitude demod, autocorrelation mode detection, H/V sync, sub-pixel
resample to an exact `width × height` grid, and multi-frame averaging. The NN
is a *replacement for the last, cosmetic step* - it turns the aligned,
noise-averaged raster into a cleaner image. So the integration points are:

1. **Raw aligned-raster output (the NN input).** The `tempest` plugin gains an
   option `deep_capture=1` that, instead of (or alongside) the grayscale
   `host.video_frame()`, emits the **aligned complex raster** - real and
   imaginary parts on the `width × height` grid, plus the detected-mode
   metadata (pixel rate, totals, fps, sync offsets, `rf_hz`). This is exactly
   the tensor the network consumes. It leaves the receiver by the existing
   recording path, not a new one:
   - reuse the **IQ recorder / SigMF** (*recorder*, `SIGMF.md`) to write the
     aligned frames as `.cf32` + a JSON sidecar of the mode metadata, OR
   - add a `host.video_frame()` variant carrying float pixels (see the video
     channel work) so a desktop tool can pull frames over the existing
     `/recordings` route.
   No new network egress; the Pi only writes files, as it already does for
   recordings.

2. **Offline reconstruction tool (first milestone, off the Pi).**
   `kraken_doa_v2/tempest/deep/` (a tool, not a live plugin) reads the exported
   frames and runs the model on a workstation/GPU. This is where training and
   experimentation happen, with no Pi or licence constraint on the *tooling*
   (kept out of the shipped binary). Output is a PNG the user views.

3. **Optional on-device enhancement (later milestone).** If a small enough
   model is trained (quantised, downscaled tiles), embed a permissive runner in
   the `tempest` plugin behind an option `deep=1`: on a key-press / button the
   plugin runs one forward pass over the current accumulated still and sends
   the result through the **same `host.video_frame()` channel** as an
   "enhanced" frame. The panel shows a Raw / Enhanced toggle. Still not
   per-frame; a single on-demand pass.

## Training data without the GPL dataset

We can generate our own paired data with the `tempest` plugin's own synthetic
generator (built for its offline tests):

- Render known content (text pages, UI screenshots, test cards) to a
  framebuffer = **ground truth**.
- Simulate the HDMI/DVI **TMDS** emanation of that framebuffer (10-bit TMDS
  symbol stream per pixel clock, differential, band-limited and AM-detected at
  a chosen harmonic) through the multi-antenna scene generator → **input**.
- The pair is automatically aligned because the simulator knows the exact
  timing. This yields unlimited simulated pairs under our own licence.
- Real captures (the honest, messy distribution) come from the receiver itself
  against the user's own monitor, ground-truth by screenshot - captured with
  the export in (1). The released Deep-TEMPEST dataset is a *reference* to
  check our simulator's realism against, not a training input we redistribute.

## Milestones

- **M0 - export.** `tempest` emits aligned complex rasters + metadata via the
  recorder. No model. (Unblocks everything; small.)
- **M1 - classical enhancement baseline.** In the offline tool: multi-frame
  averaging already in the plugin, plus Wiener/deconvolution of the known
  emanation point-spread, CLAHE, and a sharpening prior. Establishes the CER
  baseline the net must beat, with zero ML.
- **M2 - clean-room DRUNet, offline.** Implement the DRUNet forward pass from
  the paper's architecture; train on M0 simulated + real pairs; run in the
  desktop tool. Report CER vs M1 on a held-out set.
- **M3 - packaged weights + permissive runner.** Freeze a model to ONNX/gguf;
  wire a permissive runner (ggml/tract) into the offline tool; document the
  training recipe so users can retrain for their own displays.
- **M4 - optional on-device.** Only if a quantised small model runs a single
  still in acceptable time on a Pi 5: embed the runner in the plugin behind
  `deep=1`, Raw/Enhanced toggle, one pass on demand.

## Open questions to settle before M2

- Exact DRUNet input channel layout the paper uses (real+imag vs magnitude vs
  both) and whether they feed the pre-sync capture or the aligned raster -
  read from the paper/repo, re-derive, do not copy.
- Model size vs. Pi feasibility - likely M4 never ships and deep stays an
  offline, workstation feature. That is an acceptable outcome: the live plugin
  stays classical and real-time; "deep" is a power-user offline upgrade.
- Whether to depend on ONNX Runtime (heavy, MIT) or a tiny hand-rolled
  conv stack (no dependency, more code) for the clean-room runner.

## References

- arXiv:2407.09717, "Deep-TEMPEST: Using Deep Learning to Eavesdrop on HDMI
  from its Unintended Electromagnetic Emanations."
- DRUNet: Zhang et al., "Plug-and-Play Image Restoration with Deep Denoiser
  Prior," IEEE TPAMI 2021.
- Classical baseline and the shared pipeline: *tempest* (this folder), and the
  video display channel in `plugins/SDK.md`.
