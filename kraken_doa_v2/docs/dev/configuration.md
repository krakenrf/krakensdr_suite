# Configuration, MUSIC DoA, UCA element ordering

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

## Configuration

Edit `include/config.hpp`:

**Network:**
- `WEB_PORT`: Client web UI (HTTPS/WSS, default 8080)
- `DOA_HTTP_PORT`: Plain HTTP DoA value page for Android app (default 8081)
- `TCP_DATA_PORT`: Heimdall data port (default 8091)
- `TCP_CONTROL_PORT`: Heimdall control port (default 8092)

**Signal Processing:**
- `FFT_SIZE`: FFT size (default 16384)
- `MAX_CHANNELS`: Compile-time channel CEILING (8, matches heimdall's
  NUM_DEVICES). The live count is the runtime atomic `active_num_elements`,
  synced from the 8091 packet header
- `SAMPLE_RATE`: Expected sample rate (default 2.4e6)
- `AUDIO_SAMPLE_RATE`: Browser audio rate (default 48000) - **MUST match browser's native rate**
- `DECIMATION_FACTOR`: Default decimation (default 10)

**MUSIC DoA:**
- `DOA_NUM_ELEMENTS`: Compile-time CEILING on antenna elements (8). MUSIC and
  the beamformer follow the runtime count: each processing pass calls
  `syncElementCount()`, which re-reads `active_num_elements` and, on a change,
  resizes all per-element state (accumulator, covariance, steering vectors,
  MVDR/FD-DAS tables) and drops stale statistics. `DOA_DEFAULT_ELEMENTS` (5)
  is only the pre-connect default
- The UI adapts automatically: `onActiveElementsChanged()` regenerates the
  channel selector, UCA/ULA/custom diagrams, position table and SNR displays
  from the `active_elements` status field
- `DOA_BLOCK_SIZE`: Samples per block (default 256)
- `DOA_NUM_SNAPSHOTS`: Covariance snapshots (default 64)
- `DOA_ANGULAR_RESOLUTION`: Degrees per step (default 1)

**UCA element ordering (array chirality):**
- The UCA is expected to be wired **CLOCKWISE**: ANT0 on the +x axis, each
  subsequent channel stepping clockwise (0, -72, -144, -216, -288 deg on a
  5-element ring). This applies to both standard KrakenSDR arrays and the
  Wideband boards
- `uca_angle_sign()` (`include/globals.hpp`) returns `-1.0` and is the single
  choke point - it mirrors the element angle about +x (ANT0 stays put, the
  rest reverse order) so DoA OUTPUT stays unit-circle CCW. Flip it to `+1.0`
  for a counter-clockwise array, and flip `UCA_ANGLE_SIGN` in
  `kraken_doa.html` to match
- Fed by: MUSIC UCA steering vectors and the custom-position UCA default
  (`music_processor.cpp`), the beamformer geometry (`beamformer.cpp`), and in
  the UI the `drawUCADiagram()` element layout plus the default custom
  positions. ULA and user-entered CUSTOM positions are taken literally and
  are NOT mirrored

