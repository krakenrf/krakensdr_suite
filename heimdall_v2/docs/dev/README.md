# heimdall developer notes

One file per topic, moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24. Not
loaded into an AI session automatically: `heimdall_v2/CLAUDE.md` holds the rules
every session needs and points here.

| File | Topic |
|---|---|
| `architecture.md` | heimdall architecture, data flow, threads, debugging, dependencies |
| `calibration.md` | Calibration: lag servo, phase compensation, retune timing, calibration hold, IF VGA |
| `coherence-recovery.md` | Coherence-loss detection and recovery |
| `configuration.md` | heimdall configuration (config.h, ingest robustness, realtime scheduling) |
| `drag-retunes.md` | Drag retunes (fast path) and failed-retune replies |
| `fft-pffft.md` | FFT (pffft) |
| `kerberos-and-ext-noise.md` | KerberosSDR mode and the external noise source array |
| `operating-modes.md` | Operating modes (coherent / wideband scan / independent) |
| `wideband-variant.md` | KrakenSDR Wideband (downconverter) variant in heimdall |
