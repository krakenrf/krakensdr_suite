# Array calibration from ADS-B aircraft

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**✈ Array calibration from ADS-B aircraft (`src/array_cal.cpp`, sidebar "🧪 Experimental" -> "✈ Array Calibration", coherent) - EXPERIMENTAL, on the back burner:**
- Status (2026-10-09): works on synthetic data; on the live array here (205 mm,
  1090 MHz, aircraft at 0-9 deg elevation) each aircraft's response matched
  the nominal array only ~0.56 (healthy ~0.95) - steady per aircraft but
  different for aircraft in nearly the same direction (multipath / the
  antennas at low elevation), so the 88 deg lookalike lobes of the 0.876
  lambda ring often won. Wiring order and radius checked fine. Constant
  per-element offsets helped only partly (main lobe right 43 -> 54 %,
  leave-one-aircraft-out). Known open issues: `line_of_sight()` judges an
  aircraft by its STRONGEST lobe, so aircraft whose lookalike lobe won are
  left out although they have a lobe at the truth (should test "a strong
  lobe within LOS_DEG of the truth"); Apply doesn't ask before using a
  result with no cross-check / a worse one. Recording + scripts:
  /home/krakenrf/acal_capture (cap8091 capture, analyze / models /
  stability / loo .py)
- What it fixes: what the noise source can't see - per-element cable delay
  (element 0 = 0), per-element x / y position error and a rotation of the
  whole array (mounting / station heading) - in PHYSICAL units, so it holds
  at every frequency (the phase of a delay scales with f; copying 1090 MHz
  phases elsewhere was worse than nothing in the simulation). At one
  frequency an antenna's own phase looks exactly like a delay and is absorbed
  into it (carried to other frequencies scaled, i.e. wrongly - small at lower
  ones); what the model can't explain is reported per element ("Unexpl.")
- Input: `kp::Talker::lat / lon / alt_m` on packets (wire TALKER fields
  10-12, `TalkerSpan::lat...`): the adsb plugin's `packet_position` = the
  aircraft's last position moved on with GS / track / vertical rate to the
  packet's start time (airborne, fix <= `POS_AT_MAX_S` 3 s old, a velocity
  beyond 0.1 s; GNSS altitude, else barometric). TalkerDoa commits a packet
  -> `CalPacket {id, R, lat, lon, alt}` (only while collecting, `take_cal`)
  -> the rdf sampler adds the station (static location, or GPS with a
  compass, standing still - `cal_state`; height from a 3D GPS fix, else 0)
  -> `array_cal::add()`: ECEF -> east / north / up = compass azimuth +
  elevation; steering angle = heading - array offset - azimuth (rdf's
  compass = heading - displayed angle); dominant eigenvector + eigenvalue
  ratio of R. Gates: >= 5 km, <= 450 km, 2..60 deg elevation (below 2 deg:
  ground / horizon traffic - live at Auckland Airport five taxiing aircraft
  that sent airborne messages at -200 ft, bearing 246 deg, all read 161 deg
  and bent the fit: 16 deg unexplained, check worse), ratio >= 10, one packet
  per aircraft per 500 ms; 36 compass sectors of max 400 packets
  (then a random one replaced). The array: `note_array()` with
  `MUSICProcessor::nominalPositions()` (UCA / CUSTOM; ULA = not
  calibratable) - other positions = a refit of the same packets, another
  element count / array offset / static heading = start over (they are baked
  into the packets)
- Line of sight (`line_of_sight()`, before the fit): each aircraft's median
  bearing error with the NOMINAL array at its own elevation; the consensus =
  the offset aircraft in the most 10-deg DIRECTIONS agree with (+-25 deg; then
  the most aircraft) - a reflection is consistent per place, so a busy
  airport's aircraft would out-vote a few good ones by count; aircraft more
  than `LOS_DEG` 25 off it are left out whole (`left_out` [{id ICAO, err,
  n}], `aircraft_used`). Per-packet weights can't catch them: their packets
  are consistently wrong
- Fit (worker thread, every 20 s with new packets, ~1 s at the 14400-packet
  cap on a Pi 5): MUSIC's model (`phases()` = k (x cos el cos th + y cos el
  sin th + z sin el) - 2 pi f tau, c = 3e8 as MUSIC) against each packet's
  eigenvector, per-packet common phase projected out; start = rotation
  search in 2 deg steps + per-element circular-mean phase as a delay; Gauss-
  Newton MAP (priors: rotation 30 deg, delay 2 ns, position 15 mm), Cauchy
  reweighting (multipath / wrong packets). Cross-check: fitted on half the
  aircraft, the other half's bearings (operational el = 0 steering, 1 deg
  grid + parabola on the eigenvector, aircraft below 20 deg) before / after.
  Warnings: < 6 of 12 30-deg sectors covered, > 15 deg unexplained, check
  worse
- In use: `apply()` saves `array_cal.json` (cwd - Docker volume; gitignored)
  and bumps `generation()`; `corrections(n, x, y)` gives dx / dy (rotation
  included when `rotation` is on) + tau, only while `enabled` and the
  nominal positions match the calibrated ones within 0.5 mm. MUSIC
  (`updateSteeringVectors` / `2D`: corrected positions + -2 pi f tau; the
  processing pass rebuilds them when `generation()` changed;
  `isCalibrationApplied()`) and the beamformer (`computeElementPositions`,
  `element_tau_`, FD-DAS per bin) use it. Not touched by RESET_SETTINGS
- Commands (`ARRAY_CAL` in `is_query_command`, broadcasts): `ARRAY_CAL_COLLECT:0|1`
  (not remembered across restarts; 0 = a last fit), `ARRAY_CAL_RESET`,
  `ARRAY_CAL_APPLY`, `ARRAY_CAL_USE:0|1`, `ARRAY_CAL_ROTATION:0|1`,
  `ARRAY_CAL_REMOVE`, `GET_ARRAY_CAL`. Status `{"array_cal":{collecting,
  state, samples, aircraft, last_s, rejected{near, far, low, high, weak},
  sectors[36], cap, note, array{n, ok}, fitting, min_samples, fit{ok, error,
  t, age_s, samples, aircraft, aircraft_used, left_out[], left_out_n, f_mhz, n, used, sectors, el, rot, tau_ps[],
  dx_mm[], dy_mm[], resid[], resid_all, gain_db[], cv, before / after{n, med,
  p90, wrong}, warn}, applied{...fit, enabled, rotation, match}}}` every 2 s
- UI (the "🧪 Experimental" sidebar section `#exp-section` at the bottom,
  just above Open Heimdall Server / Reset Settings - collapsed, coherent
  only; its header carries the calibration badge `#acal-badge`, "in use" /
  "collecting", so an active calibration shows while collapsed; block
  `#acal-section`, `acal*` in kraken_doa.html): the
  calibration in use (Use / Include the rotation / Remove, its check), Collect
  / Reset, why packets aren't used (`ACAL_STATE`), a 36-sector coverage ring,
  the latest result (check, rotation, per element delay - also as coax mm at
  VF 0.66 -, x / y, unexplained, gain dB - shown only, gains aren't
  corrected) + Apply. Boxes re-render only when their HTML changes (the
  result's age has its own span): a re-render under the pointer swallowed
  clicks
- Tests (scratchpad acal/): `t_acal.cpp` (array_cal + MUSICProcessor on
  synthetic packets from a 205 mm UCA with rotation 2.5 deg, 12-60 ps
  delays, 2-4 mm position errors, 2-5 deg antenna phases: positions within
  0.25 mm, delays = truth incl. the antenna phases, MUSIC 1090 MHz median
  error 3.0 -> 0.09 deg, 433 MHz 2.9 -> 0.74 (0.35 without antenna
  differences), save / load, rotation off, other radius = not used, refit /
  start-over rules; `t_acal airport`: 5 good aircraft + 6 clustered at 246
  deg reflected from 161 deg + 2 on the ground -> the 6 left out, the 2
  skipped, positions within 0.25 mm); `adsb_sim.py` (Mode S generator: DF17 ident / CPR
  position / velocity, PPM at 24 MHz integrated to 2.4, the same array
  errors; `file` for `decoder --file`, `serve` = a fake heimdall on 8091):
  plugin packet positions median 2.4 m off the truth; `t_e2e.py` against the
  test build (ports 1808x, `KRAKEN_PLUGIN_DIR` = the real plugins): 30
  aircraft, fit within 0.25 mm / 0.1 ps, live per-aircraft DF error (below
  20 deg) 3.05 -> 0.22 deg, off again 3.44; `t_acal_ui.py` (headless
  Chromium against it)

