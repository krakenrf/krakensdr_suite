# Antenna array calculator

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Antenna array calculator (`array_calculator.html`):**
- Standalone page (no external resources), served by its own route in
  `websocket_server.cpp` (read per request like kraken_doa.html, so edits
  need no rebuild; the ROUTE itself needs a rebuild). Opened by the
  "📐 Array Calculator" button in the MUSIC DoA box (`openArrayCalculator()`),
  which passes the receiver's state as URL params: `n` elements, `f` MHz,
  `topo` UCA|ULA|CUSTOM, `r` radius mm, `s` spacing mm
- Port of krakensdr_docs `Antenna_Array_Size_Calculator.xlsx` (same formulas,
  but λ = c/f instead of 300/f, so ~0.07% off): UCA radius = m·λ/(2 sin(π/N)),
  ULA length = (N-1)·m·λ, est. resolution = deg(1.22/aperture_in_λ)/10 with
  the sheet's aperture definitions; spacing multipliers 0.5..0.1, ≤ 25° = ok.
  "Patch / 3D" tab layouts are sized so the largest nearest-neighbour
  spacing = m·λ: 3D (ring + centre mast, staggered, two stacked rings - add
  an elevation estimate) and flat patch arrays (`p-*`: polygon + centre with
  side or corner forward - square / diamond at N=5 -, 2×k grid, two offset
  rows (triangular lattice), L-shape, ring). Patch arrays are always upright
  panels facing the horizon (x = 0, numbered clockwise seen from behind):
  azimuth + elevation, but front/back mirror-ambiguous. (A flat board facing
  the sky was dropped - z = 0 makes the receiver's CUSTOM MUSIC azimuth-only.)
  Patch layouts keep their
  element spacing across layout / count changes
- The frequency marker on the "Your array" chart is draggable (x axis
  frozen during the drag); the MHz field steps 0.1 MHz from any typed value
  (no `min` attribute, so the step base is the value attribute, which the
  page keeps in sync)
- Coordinates use the receiver's frame (x forward = ANT0, y left, z up, UCA
  clockwise). "Use this array in the DoA receiver" calls
  `window.opener.applyArrayFromCalculator(cfg)` in kraken_doa.html, which
  sets UCA radius / ULA spacing / the Patch / 3D layout through the normal UI
  paths (so they are sent and persisted); refused if N differs from the live
  element count. The receiver passes its layout as URL params `shape`, `ls`
  (size mm), `lh` (height mm)

