# Heimdall v2 TCP Control Protocol — Port 8092 (`TCP_CONTROL_PORT`)

Source of truth: `/home/krakenrf/krakensdr_v2/heimdall_v2/src/net/tcp_control_server.cpp` (868 lines), `.hpp`, plus `src/main.cpp` (broadcaster), `src/web/web_server.cpp` (identical cooldown flow), `src/dsp/compensation.cpp` (cooldown expiry), `src/sdr/sdr_init.cpp` (`update_sdr_settings`, `handle_settings_change`), `src/core/types.hpp` (enums), `src/net/tcp_data_server.cpp` (where `phase_state` actually lives).

---

## 1. Wire framing

**Transport**: plain TCP, `INADDR_ANY:8092`, up to 5 pending in `listen()`. Multiple simultaneous clients supported; server `select()`s with a 100 ms timeout.

**Client → server (commands)**: JSON objects. The parser (`handle_client_data`, lines 196–263) is **stream-oriented, not line-oriented**: it accumulates a per-client `rx_buffer` and extracts every complete top-level `{...}` object using quote-aware brace matching. Consequences:
- Newlines between commands are optional (whitespace `\n \r \t` between objects is skipped).
- Multiple commands coalesced into one TCP segment are each processed.
- A command split across `recv()` calls is reassembled.
- Garbage before a `{` is dropped up to the next `\n`. An unterminated object > 65536 bytes clears the buffer.
- **Command matching is substring-based** (`json_str.find("\"set_frequency\"")` etc.) — the server never parses a `"command"` key properly. Convention: `{"command":"set_frequency","frequency":100000000}`. Any JSON containing the quoted command name works; **only the first matching command name in the object is handled** (matching order = code order below).

**Server → client**: two interleaved kinds of messages on the same socket, each a single-line JSON object terminated by `"\n"`:
1. **Command responses** — sent immediately after each processed command, `response + "\n"`. Always contain `"status":"success"` or `"status":"error"` (+ `"message"` on error). Parse exception → `{"status":"error","message":"<what>"}\n`.
2. **Unsolicited status broadcasts** — from the `tcp_status_broadcaster` thread (`src/main.cpp:74–84`): `broadcast_status()` every **500 ms (2 Hz)**, terminated `"}\n"`. Skipped entirely while no clients are connected. A client must therefore read line-by-line and distinguish: broadcasts have a top-level `"settings"` key and no `"status"` key; responses have `"status"`.

Unknown command → `{"status":"error","message":"Unknown command"}`.
**Note:** `get_status` (mentioned in docs/CLAUDE.md) is **not implemented** — it returns the Unknown-command error. The 2 Hz broadcast is the status mechanism.

---

## 2. Commands (exhaustive, in matching order)

### 2.1 `set_frequency` (line 267)
```json
{"command":"set_frequency","frequency":100000000}
```
- `frequency`: Hz, parsed with `std::stod` then cast to `uint64_t` (so `1.0e8` works). **RF is the true RF** even in the downconverter variant.
- Valid range: `24000000`–`1766000000` Hz normally; in the `--wideband` downconverter variant the union of all mixer sides via `downconverter_rf_union_range()` (24–6668 MHz).
- Success: `{"status":"success","frequency":100000000}`
- Out of range: `{"status":"error","message":"Frequency out of range (24-1766 MHz)"}` (numbers reflect the active range in MHz).
- Missing/garbled field: `{"status":"error","message":"Invalid frequency format"}`

**Side effects (coherent mode):**
1. `update_sdr_settings(frequency, -999, devices)` retunes all tuners in parallel (gain `-999` = "don't touch gain"). Returns `changed=true` only if `|new−old| > 1000` Hz — a retune of ≤ 1 kHz does NOT trigger recalibration (and in the wideband variant, only if the LO reprogram succeeded).
2. If `changed` and **not** `recovery_in_progress`:
   - `set_bias_tee_all_devices(false, ...)` — **noise source OFF** (GPIO0; "bias tee" here = the calibration noise source, not antenna bias tees).
   - Under `phase_compensation->state_mutex`: `last_frequency_change = now`, `cooldown_active = true`, `state = PhaseCompensatorState::WAITING_FOR_STABILITY` (numeric **5**), `compensation_applied=false`, counters zeroed, `per_bin_measured=false`, `per_bin_cal.ready=false`, and the compensation vector reset to identity `Complex(1,0)` for every channel except `REF_CHANNEL`.
   - Log: `"Frequency changed: entering <N>ms cooldown (bias tee OFF)"`.
3. **Every subsequent `set_frequency` during the cooldown restarts the timer** (`last_frequency_change` is re-stamped) — this is deliberate, for drag-to-scroll scanning.
4. If `recovery_in_progress` (coherence recovery running): hardware is retuned but the cooldown/recal override is skipped; the in-flight recovery recalibrates at the new frequency.

**Wideband-scan mode** (`OperatingMode::WIDEBAND_SCAN`, the tuner-spread mode): sets `current_frequency` and calls `setup_wideband_frequencies()` directly — **no cooldown, no recalibration**.

### 2.2 `set_mixer_side` (line 353) — downconverter variant only
```json
{"command":"set_mixer_side","side":"high"}
```
- `side`: exactly `"high"`, `"low"` or `"below"` (matched as literal `"side":"high"` etc.).
- Errors: not in wideband variant → `"Not in wideband variant mode (start with --wideband)"`; bad value → `"side must be \"high\", \"low\" or \"below\""`; unreachable RF → `"<side> side cannot reach RF <n> MHz (covers <a>-<b> MHz)"`; HID failure → `"Failed to program downconverter LO"`.
- No-op if already on that side: `{"status":"success","side":"high","lo_hz":<LO>}`.
- Success: `{"status":"success","side":"high","frequency":<RF>,"lo_hz":<LO>}`.
- Side effects: **identical cooldown + phase-recal flow as `set_frequency`** (noise source off, `WAITING_FOR_STABILITY`, identity vector), skipped during recovery.

### 2.3 `set_array` (line 438) — downconverter variant only
```json
{"command":"set_array","array":0}
```
- `array`: `0`=outer, `1`=center, `2`=inner ring. Range error: `"array must be 0 (outer), 1 (center) or 2 (inner)"`.
- No-op if unchanged: `{"status":"success","array":0}` without recal.
- On change: throws RF switches (`wideband_set_noise_path`), then the **same cooldown + phase-recal flow** as `set_frequency`. Success: `{"status":"success","array":<n>}`.
- Note: the next retune overrides this (ring follows frequency automatically); manual-testing command.

### 2.4 `set_lo_current` (line 508) — downconverter variant only
```json
{"command":"set_lo_current","current":3}
```
- `current`: 0–7. Errors: `"current must be 0-7"`, `"Failed to program LO current"`.
- Success: `{"status":"success","lo_current":3}`. **No cooldown / no recalibration** (shared LO = common-mode).

### 2.5 `set_stability_delay` (line 535)
```json
{"command":"set_stability_delay","delay_ms":3000}
```
- `delay_ms`: 100–10000 ms. Sets `phase_compensation->stability_delay_override_ms` (default `STABILITY_DELAY_MS = 3000`, types.hpp:181). This is the cooldown length used by frequency/gain/side/array changes and reported in `cooldown_remaining_ms`.
- Success: `{"status":"success","stability_delay_ms":3000}`; range error: `"Delay out of range (100-10000 ms)"`.

### 2.6 `set_gain` (line 555)
```json
{"command":"set_gain","gain":49.6}
```
- `gain`: **dB as a float** (not tenths). Parsing: `gain_db < 0` → `gain = -1` = **AGC/auto mode**; otherwise `gain = int(gain_db * 10)` (tenths of dB), accepted iff `0 <= gain <= 500` → **0–50.0 dB**. Any value in tenths-of-dB granularity is passed to `rtlsdr_set_tuner_gain()` as-is (no snapping to R820T gain steps in this code; the driver handles it).
- Out of range: `{"status":"error","message":"Gain out of range (0-50 dB or auto)"}`.
- Success: `{"status":"success","gain":49.600000}` (float via `std::to_string`).
- Side effects: `update_sdr_settings(0, gain, devices)` (frequency 0 = unchanged; `changed` iff `gain != current_gain`). On change, coherent mode, not recovering: **exactly the same cooldown flow as `set_frequency`** — noise source OFF, `WAITING_FOR_STABILITY` (5), identity vector, timer restart. Log says `"Gain changed: entering 3s cooldown (bias tee OFF)"` (message hardcodes "3s" but the actual delay is `stability_delay_override_ms`). In wideband-scan mode: no cooldown. During recovery: deferred to the full recal.
- Repeating the same gain returns success without side effects (`changed=false`).

### 2.7 `set_rtl_tcp_channel` (line 619)
```json
{"command":"set_rtl_tcp_channel","channel":0}
```
- `channel`: 0 … `NUM_DEVICES-1`. Selects which channel the RTL-TCP server (port 1234) streams. Success: `{"status":"success","rtl_tcp_channel":0}`; error: `"Channel out of range (0-4)"`. No calibration impact.

### 2.8 `set_wideband_mode` (line 644) — tuner-spread scan mode (NOT the downconverter variant)
```json
{"command":"set_wideband_mode","enable":true,"base_frequency":100000000}
```
- `enable`: `true`/`false`. Optional `base_frequency` (Hz, `uint32_t`): applied even if the mode was already enabled (used by the discrete scanner). Refused while the downconverter variant is active (inside `set_wideband_mode()`).
- Success: `{"status":"success","wideband_enabled":true}` or `{"status":"success","message":"Already in requested mode"}`.

### 2.9 `set_wideband_frequencies` (line 688)
```json
{"command":"set_wideband_frequencies","base_frequency":100000000}
```
- Requires wideband-scan mode: else `"Not in wideband scan mode"`. Success: `{"status":"success","base_frequency":100000000}`.

### 2.10 `set_wideband_edge_clip` (line 709)
```json
{"command":"set_wideband_edge_clip","edge_clip":0.8}
```
- Float, clamped to 0.1–1.0. Success: `{"status":"success","edge_clip":0.800000}`. Reconfigures tuner spacing if in wideband-scan mode.

### 2.11 `get_wideband_status` (line 739)
Response:
```json
{"status":"success","operating_mode":"coherent|wideband","wideband_enabled":false}
```
plus, when enabled: `"tuner_frequencies":[...]`, `"usable_bandwidth_per_tuner":<Hz>`, `"total_coverage":<Hz>`, `"edge_clip":<f>`.

### 2.12 `reset_lag_compensation` (line 766)
```json
{"command":"reset_lag_compensation"}
```
→ `reset_lag_compensation_all_channels()`; response `{"status":"success","message":"Lag compensation reset for all channels"}`.

### 2.13 `configure_scanner` (line 772)
```json
{"command":"configure_scanner","frequency_groups":[100000000,433000000],"dwell_time_ms":1000}
```
- `dwell_time_ms` clamped to minimum 500. `frequency_groups`: flat array of Hz (uint64). Empty → `"No valid frequencies provided"`.
- Success: `{"status":"success","num_groups":2,"dwell_time_ms":1000}`.

### 2.14 `start_scanner` (line 830) / `stop_scanner` (line 844) / `get_scanner_status` (line 851)
```json
{"command":"start_scanner"}
```
- `start_scanner` errors if unconfigured: `"Scanner not configured. Use configure_scanner first."`; success `{"status":"success","message":"Scanner started"}`.
- `stop_scanner` → `{"status":"success","message":"Scanner stopped"}`.
- `get_scanner_status` →
```json
{"status":"success","enabled":false,"num_groups":2,"current_group_index":0,"frequency_change_counter":7,"dwell_time_ms":1000,"current_frequency":100000000}
```
(`current_frequency` only when `num_groups > 0`.)

---

## 3. The 2 Hz status broadcast (exact schema, `broadcast_status()` lines 53–147)

One line per broadcast, terminated `"}\n"`:

```json
{
  "settings": {
    "center_freq": 100000000,        // uint64 Hz; TRUE RF in downconverter variant
    "gain": 49.6,                    // dB (current_gain/10.0), or -1 = auto/AGC
    "sample_rate": 2400000,          // compile-time SAMPLE_RATE
    "rtl_tcp_channel": 0
  },
  "num_channels": 5,                 // active_num_elements
  "operating_mode": "coherent",      // "wideband" iff OperatingMode::WIDEBAND_SCAN, else "coherent"
  "wideband_enabled": false,         // wideband_config.enabled (tuner-spread scan)

  // ONLY when downconverter (--wideband variant) is enabled:
  "downconverter": {
    "enabled": true,
    "if_hz": 1268000000,             // WB_VARIANT_IF_HZ
    "side": "high",                  // "high" | "low" | "below"
    "lo_hz": 1368000000,
    "array": 0,                      // 0=outer, 1=center, 2=inner ring
    "lo_current": 3                  // 0-7
  },

  // ONLY in wideband-scan mode:
  "tuner_frequencies": [ ... ],      // per-tuner Hz, num_channels entries

  // Always present when phase_compensation exists (i.e. normally always):
  "cooldown_active": false,          // true during the post-retune/post-gain cooldown
  "cooldown_remaining_ms": 0,        // max(0, stability_delay_override_ms - elapsed since last change); 0 when inactive
  "channel_comp": [                  // live applied calibration vector, num_channels entries
    {"amp_db": 0, "phase_deg": 0},   // ref channel always identity; identity (0/0) until first apply
    {"amp_db": -1.2, "phase_deg": 37.5}
  ],

  // Always present:
  "coherence_events": 0,             // running count of detected desync events
  "recovering": false,               // true while a coherence recovery (flush + full recal) is in flight
  "lag_no_peak": []                  // channels whose lag calibration finds no correlation peak (below)
}
```

`lag_no_peak` (added 2026-11-13): the channel numbers whose lag calibration has
servoed for `LAG_NO_PEAK_WARN_S` (60 s) without a lock while the median
correlation peak / mean magnitude of its last 32 readings stayed below
`LAG_NO_PEAK_RATIO` (8) - no peak in the correlator's +-16384-sample range,
typically because the dongles' streams started further apart than that; the
calibration then never converges on its own. A channel enters the list together
with heimdall's one-line stderr warning and leaves it when it locks or a new
calibration starts (Full Recalibration / `reset_lag_compensation`, an
element-count change). Always `[]` outside coherent mode. The remedy is a Full
Recalibration (its buffer flush re-pairs the streams), else a heimdall restart.
The web STATE message carries the same field.

**Important — what is NOT in this JSON:** the numeric `phase_state` and per-channel lag state are **not** broadcast on port 8092. They travel in the **data-port packet header (port 8091)**, big-endian uint32s (`tcp_data_server.cpp:88–166`):

```
magic(4)=0x4D434851 'MCHQ' | num_channels(4) | num_samples(4) |
phase_state(4) | noise_source(4) |
freq_change_counter(4) | current_group_index(4) | retuning_in_progress(4) |
per-channel { freq(float32 LE, native memcpy) , gain_db(float32 LE) } × N |
then uint8 interleaved IQ per channel
```

- `phase_state` = `static_cast<uint32_t>(PhaseCompensatorState)`:
  - `0` = `WAITING_FOR_LAG_COMPLETION`
  - `1` = `MEASURING_INITIAL_PHASE`
  - `2` = `APPLYING_COMPENSATION` (legacy, no longer set; funneled to verify)
  - `3` = `VERIFYING_CONVERGENCE`
  - `4` = `CONVERGED` ← the only "fully calibrated" state
  - `5` = `WAITING_FOR_STABILITY` (retune/gain cooldown)
  - `6` = `MEASURING_PER_BIN` (optional per-bin equalizer measurement)
- `noise_source` = `bias_tee_enabled ? 1 : 0` (1 = calibration noise source is routed in; data is NOT antenna signal).
- Lag states (`LagCompensatorState`, types.hpp:22): `0`=`MEASURING`, `1`=`SERVOING`, `2`=`CONVERGED` — visible only in the web UI/WebSocket, not on 8092/8091.

---

## 4. Client protocol after a frequency (or gain / mixer-side / array) change

1. Send `{"command":"set_frequency","frequency":F}`; read the immediate `{"status":"success","frequency":F}` line.
2. Hardware retunes right away; the server enters the cooldown: noise source OFF, `phase_state` → **5** (`WAITING_FOR_STABILITY`), compensation vector reset to **identity**, per-bin EQ disabled. The 8092 broadcasts flip to `"cooldown_active":true` with `"cooldown_remaining_ms"` counting down from `stability_delay_override_ms` (default **3000 ms**, settable 100–10000 via `set_stability_delay`); `"channel_comp"` shows 0 dB / 0°.
3. Any further `set_frequency`/`set_gain` during the cooldown **restarts** the timer (rapid scrolling never triggers calibration mid-drag).
4. When the timer expires (checked in the compensation thread, `compensation.cpp:996–1023`): `cooldown_active=false`, then `handle_settings_change()` (`sdr_init.cpp:446+`) — **noise source ON**, `phase_state` → **1** (`MEASURING_INITIAL_PHASE`); lag states stay CONVERGED (phase-only recal). It then walks 1 → 3 (`VERIFYING_CONVERGENCE`) → optionally 6 → **4** (`CONVERGED`), at which point `complete_phase_calibration_locked()` (`compensation.cpp:24–47`) switches the **noise source OFF** and auto-disables server-side FFT.
5. **Data-validity rule for a DoA client**: samples on port 8091 are antenna data with valid calibration only when the packet header has `phase_state == 4` **AND** `noise_source == 0`. During states 1/2/3/6 (or whenever `noise_source == 1`) the channels carry calibration noise; during state 5 the calibration vector is identity (uncalibrated antenna data — fine for spectrum display, wrong for DoA). Do **not** treat `phase_state < 5` alone as "frozen" (per project memory: gate on the noise source; 4 is converged).
6. During the cooldown itself data flows continuously (identity compensation) — spectrum display keeps working; only DoA output should be gated.
7. If `"recovering":true` (coherence recovery), frequency/gain changes still retune hardware but skip the cooldown; wait for `recovering:false` + `phase_state==4`.
8. **Discrete-scanner semantics (separate mechanism)**: while the server-side scanner runs, `retuning_in_progress` (header uint32, also mirrored in `get_scanner_status`) is `1` from just before a hop until after the settling flush (default `settling_time_ms` 150 ms) — the client must discard packets while it is `1` and use `freq_change_counter` (increments once per hop) / `current_group_index` to detect hops and reset FFT averaging.

---

## 5. Gain rules summary

- Wire value: **dB float** in `"gain"`. Negative → auto (AGC on all tuners, `rtlsdr_set_tuner_gain_mode(dev,0)`). `0.0`–`50.0` dB → manual, stored internally as tenths (`0`–`500`), applied via `rtlsdr_set_tuner_gain_mode(dev,1)` + `rtlsdr_set_tuner_gain(dev, tenths)`.
- Status broadcast reports `"gain"` in dB, `-1` meaning auto; the 8091 header carries `gain_db` as float32 per channel (`-1.0` = auto), same value for all channels.
- A gain change (any delta, including auto↔manual) triggers the full cooldown → phase+amplitude recalibration flow (the eigenvector cal equalizes amplitude too, visible afterwards in `"channel_comp"` `amp_db`).
- Same gain resent = no-op success. Via the web UI a gain-only change skips the cooldown and recalibrates immediately (`handle_settings_change()`, web_server.cpp:313–316); via port 8092 it uses the cooldown.