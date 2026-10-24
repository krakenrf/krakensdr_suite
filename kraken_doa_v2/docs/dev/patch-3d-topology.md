# Patch / 3D topology

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Patch / 3D topology (`TOPOLOGY:PATCH3D`):**
- UI topology button for the calculator's "Patch / 3D" layouts (upright patch
  panels: square/diamond + centre, grid, offset rows, L-shape, ring; 3D: ring +
  centre mast, staggered ring, stacked rings). The backend runs it as CUSTOM
  (`PATCH3D` maps to `ArrayTopology::CUSTOM`); the page generates the
  positions (`layoutPositions()` in kraken_doa.html - keep it identical to the
  calculator's geometry) and sends `CUSTOM_POSITIONS:`
- The layout is UI state: `ARRAY_LAYOUT:shape,elements,size_mm,height_mm`
  (whitelisted shape, persisted, replayed to browsers; no effect on the DSP).
  Picking a layout also sets the custom Direction (`CUSTOM_MODE:`): Forward
  only for patch panels (front/back ambiguous), Both for 3D
- An element-count change does NOT regenerate the positions (the physical
  array didn't change): the panel warns and offers "Re-apply"
- Custom front/back truncation (`CUSTOM_MODE:FORWARD|BACKWARD|BOTH`, separate
  from `ULA_MODE:`) is applied to the 2D spectrum BEFORE the az/el peak search

