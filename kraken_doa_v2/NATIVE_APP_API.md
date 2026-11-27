# Heimdall DoA Client — Native App / WebSocket API

This document is for developers building a native client (Android/iOS app,
desktop tool, script) that controls the DoA client and consumes its live
streams. **The native API is the same WebSocket the browser UI uses — there is
no separate port or protocol.** Anything the web UI can do, a native client can
do by speaking this protocol.

- **Endpoint:** `wss://<host>:8080/ws` (TLS/WSS, binary + text frames)
- **Direction:** text commands go *up*; binary stream frames + JSON status come
  *down*.
- **State model:** the backend is the single source of truth. Every
  state-changing command is echoed to *all* connected clients as
  `{"sync_cmd":"<command>"}`, so multiple clients (e.g. the browser and your
  app) stay in sync automatically.

---

## 1. TLS / certificate (pin the key or the receiver's CA)

Port 8080's certificate is signed by the receiver's own small certificate
authority (`docs/dev/https-certificate.md`): `krakensdr-ca.crt` (CN
`KrakenSDR <host> CA <date>`, `O=KrakenSDR receiver`, name-constrained to
the receiver's names) signs `server.crt` (`CN=<host>`, `O=KrakenSDR
receiver`; the file holds the server certificate followed by the CA).

- Both are made per receiver (no key is shipped), so there is no fixed pin
  to put in the app: use **trust-on-first-use (TOFU)** - accept on the first
  connect, remember, compare from then on (the SSH model).
- **Remember the server key or the CA, never the server certificate.** The
  server certificate is re-issued whenever the receiver's names / addresses
  change or it gets old (825 days, renewed 60 days before), always for the
  SAME key (`server.key` is kept). Its fingerprint therefore changes; these
  don't:
  - the **SPKI pin**: SHA-256 of the server certificate's
    SubjectPublicKeyInfo - the same across every re-issue:
    `openssl x509 -in server.crt -noout -pubkey | openssl pkey -pubin -outform DER | openssl dgst -sha256 -binary | base64`
    (the HPKP / OkHttp `CertificatePinner` "sha256/..." form);
  - or the **CA certificate**: `GET https://<host>:8080/krakensdr-ca.crt`
    (no token, `application/x-x509-ca-cert`), trusted as the only anchor
    for this receiver. It changes only when the receiver makes a new CA (a
    new host name, the narrow / wide scope switched, about every nine
    years, or by hand) - the user then installs the new one on every device
    as well, so treat a new CA like a first connect (ask the user).
  A changed SPKI with the same CA means the receiver's key was replaced
  (rare: deleted by hand); a changed SPKI and CA = treat as a different
  receiver.
  - **iOS:** add an ATS exception for the host and implement
    `URLSessionDelegate urlSession(_:didReceive:completionHandler:)`: compare
    the SPKI hash of the leaf (`SecTrustCopyCertificateChain`,
    `SecCertificateCopyKey`, `SecKeyCopyExternalRepresentation` + the SPKI
    header for the key type), or set the stored CA as the only anchor
    (`SecTrustSetAnchorCertificates` + `SecTrustSetAnchorCertificatesOnly`)
    and evaluate.
  - **Android:** OkHttp `CertificatePinner` with the `sha256/` SPKI pin, or a
    `network-security-config` / custom `X509TrustManager` whose trust anchor
    is the stored CA.
- Host name checks: the certificate names `<host>`, `<host>.local`,
  `<host>.<search domain>`, `localhost` and, in the receiver's wide scope
  only, its private LAN addresses; connecting by IP in narrow scope fails a
  normal host name check (with a pin, skip the host name check, not the pin).

---

## 2. Connection handshake

```
1. Open WSS to wss://<host>:8080/ws   (binaryType = arraybuffer)
2. AUTH      (only if a token is configured — see below)
3. SUBSCRIBE (optional — choose which streams you receive)
4. ... send commands / receive frames ...
```

### 2a. Authentication (`AUTH:`)

Auth is **optional and fail-open**: if the server has no token configured, no
authentication is required and you may skip this step entirely.

- On connect, if a token is required the server sends
  `{"auth_required":true}`.
- Send **`AUTH:<token>`** as your **first** text message (before any command).
- On success: the server subscribes you and replays current state (see §3); no
  explicit "ok" is sent — the state replay is your signal.
- On failure: the server sends `{"auth_error":"invalid token"}` and closes the
  socket with code **1008** (policy violation).

If no token is configured, sending `AUTH:<anything>` is a harmless no-op.

With a token configured: send `AUTH:` within **10 s** of connecting (the
socket is closed otherwise); at most 8 connections may be waiting to
authenticate at once (`1013` otherwise); and an IP that sends 5 wrong tokens
within 60 s is refused (`1008 too many failed attempts`) for the rest of that
window.

Every connection must address the device by an IP, `localhost`, its hostname
(`<host>`, `<host>.local`, ...) or a name in `KRAKEN_ALLOWED_HOSTS`, and a
browser connection's `Origin` must match its `Host` - otherwise the upgrade is
answered `403`.

> The token is configured server-side via the `KRAKEN_API_TOKEN` environment
> variable or an `api_token` file next to the binary. Use a long random
> alphanumeric token. It is NOT embedded in the served page (anyone who can
> reach port 8080 could read it there): the browser UI asks the user for it
> once when the server sends `auth_required` and remembers it in that
> browser's localStorage.
>
> With a token configured, `GET /recordings/<file>` also requires it, in an
> `X-Kraken-Token: <token>` request header (401 otherwise).

### 2b. Stream subscription (`SUBSCRIBE:`)

By default a client receives **all** streams. To save bandwidth (e.g. a phone
that doesn't need the spectrum at all), send:

```
SUBSCRIBE:FFT,AUDIO,DOA      # any subset; or "ALL"; or empty for control-only
```

- Selectable streams: **`FFT`**, **`AUDIO`**, **`DOA`**.
- The control channel (`sync_cmd` echoes, scanner/DoA state, `system_status`)
  is **always** delivered regardless of subscription — it's required for
  correct multi-client state sync.
- The server acks with `{"subscribed":{"fft":<bool>,"audio":<bool>,"doa":<bool>}}`.
- You may re-`SUBSCRIBE` at any time to change the set.

---

## 3. State replay on (re)connect

Immediately after subscribing (on connect when fail-open, or right after a
successful `AUTH`), the server pushes the current state so your UI can
initialize:

- `{"scanner_sync":{...}}` — current scanner state.
- A series of `{"sync_cmd":"<command>"}` messages — the latest value of each
  replayed setting (topology, radius, MUSIC params, FM/DoA toggles, etc.).

Apply these the same way you'd apply a user action. Frequency/gain/channel and
averaging are *not* replayed here — they arrive continuously in the FFT frame
header (§5.1).

---

## 4. Control commands (text, sent up)

Send commands as plain UTF-8 text frames. Format is `KEY:value` (or a bare
keyword for actions). All settings exposed in the web UI are covered. Numbers
are decimal; booleans are `1`/`0` unless noted.

### Tuning / RF
| Command | Meaning |
|---|---|
| `FREQ:<MHz>` | Set center frequency (float MHz). Exits wideband mode if active. |
| `WIDEBAND_FREQ:<MHz>` | Set center frequency while staying in wideband mode. |
| `GAIN:<dB>` | Set RF gain (float dB) for the active channel. |
| `CHANNEL:<n>` | Select active channel (display + FM), `0..MAX_CHANNELS-1`. |
| `FREQ_OFFSET:<kHz>` / `FREQ_OFFSET_1:<kHz>` / `FREQ_OFFSET_2:<kHz>` | Per-decimator offset by position. |

### Demodulation / audio
| Command | Meaning |
|---|---|
| `FM:<0\|1>` | Enable/disable FM audio. |
| `DEMOD_MODE:<mode>` | Global demod mode (per-decimator form is `SET_DECIMATOR_DEMOD`). |
| `BANDWIDTH:<idx>` | Global bandwidth index (per-decimator form is `SET_DECIMATOR_BW`). |
| `AVG:<alpha>` | FFT averaging coefficient (0..1). |

### Decimators
| Command | Meaning |
|---|---|
| `ADD_DECIMATOR` | Add a decimator. |
| `REMOVE_DECIMATOR:<id>` | Remove decimator by id. |
| `SET_DECIMATOR_FREQ:<id>:<kHz>` | Set a decimator's frequency offset from the center (kHz, float). |
| `SET_DECIMATOR_BW:<id>:<idx>` | Set a decimator's bandwidth (index into `BANDWIDTH_OPTIONS`). |
| `SET_DECIMATOR_DEMOD:<id>:<mode>` | Set a decimator's demod mode. |
| `SET_FM_DECIMATOR:<id>` | Choose which decimator feeds FM audio. |
| `GET_DECIMATOR_INFO` | Query → replies `{"decimator_info":[...]}`. |

### DoA / array geometry
| Command | Meaning |
|---|---|
| `DOA:<0\|1>` | Enable/disable MUSIC DoA. |
| `DOA_DEBUG:<0\|1>` | Debug: DoA (type 3, the bearing spectrum, DOA_value.html) keeps running while heimdall's noise source is on - for testing. One flag for every client, echoed as a `sync_cmd` (also on connect); never saved, off at every start. The DoA log, web mapper and the other bearing stores keep their hold. |
| `TOPOLOGY:<UCA\|ULA\|CUSTOM>` | Array topology. |
| `RADIUS:<mm>` | UCA radius. |
| `SPACING:<mm>` | ULA element spacing. |
| `CUSTOM_POSITIONS:<...>` | Custom element positions. |
| `ELEVATION_RESOLUTION:<deg>` | Elevation scan resolution (3D arrays). |
| `MUSIC_NUM_SNAPSHOTS:<n>` / `MUSIC_SNAPSHOT_LENGTH:<n>` | Covariance snapshots / length. |
| `MUSIC_SIGNAL_SOURCES:<n>` | Expected number of signal sources (signal-subspace dim). Clamped to `0..elements-1`; `0` = automatic per-frame estimation (an eigenvalue counts as a source when ≥6 dB above the noise-eigenvalue mean AND within 12 dB of the dominant eigenvalue, with 3-frame hysteresis) (the live estimate is reported as `est_sources` in `system_status.decimator_squelch`). |
| `ULA_MODE:<FORWARD\|BACKWARD\|BOTH>` | ULA front/back truncation. Resolves the linear-array mirror ambiguity by zeroing one half-plane of the pseudospectrum (forward = within ±90° of 0°). No effect unless topology is `ULA`. Default `BOTH`. |
| `ARRAY_OFFSET:<deg>` | Array orientation offset (0–359) added to the reported bearing; rotates the pseudospectrum so it aligns with the array's physical mounting. Applies to all topologies. Default `0`. |
| `MUSIC_FB_AVERAGING:<0\|1>` | Forward-backward covariance averaging. Doubles the effective snapshot support and partially decorrelates coherent multipath. Only valid for a centro-symmetric array, so it is applied only while topology is `ULA` (inert otherwise). Default `0`. |
| `MUSIC_COVARIANCE_ALPHA:<a>` | Temporal covariance smoothing across updates: `R_avg = (1-a)*R_avg + a*R_frame`. `a` is the newest-frame weight, clamped to `0.05..1.0`; `1.0` = off (default). The average resets on retune/topology/config changes. |
| `EDGE_CLIP:<pct>` | Wideband stitch edge clip. |

### Beamforming
| Command | Meaning |
|---|---|
| `BEAMFORMING:<0\|1>` | Enable/disable. |
| `BEAMFORMING_MODE:<DAS\|FREQ_DAS\|MVDR\|DIVERSITY>` | Mode. |
| `MVDR_DIAGONAL_LOADING:<a>` / `MVDR_SNAPSHOTS:<n>` | MVDR regularization / snapshots. |
| `MANUAL_STEERING:<0\|1>` / `STEERING_ANGLE:<deg>` | Manual steer on/off + angle. |

### Squelch
| Command | Meaning |
|---|---|
| `DEC_SQUELCH_ENABLE:<id>:<0\|1>` / `DEC_SQUELCH_LEVEL:<id>:<v>` | Per-decimator squelch enable / FFT-peak threshold (dB). |
| `DEC_SQUELCH_METHOD:<id>:<FFT\|EIGEN\|EIGEN_AUTO>` | Per-decimator squelch method: FFT peak, eigenvalue ratio with manual threshold, or auto eigenvalue (self-learned threshold = idle-λ floor × 2.5, min 1.3; floor learning is FFT-gated — frozen while an in-band FFT peak ≥ 7 dB above the normalized noise floor is visible (DC bins excluded, and only when valid FFT data exists) — so landing on a continuous transmission keeps the threshold unlearned (0 = open) until the first quiet moment, while elevated coherent noise floors with no FFT peak are learned and squelched; relearns whenever the VFO frequency moves). The λ ratio comes from that decimator's MUSIC covariance. While the squelch is CLOSED the published DoA output freezes at the last open frame for every method, including with beamforming active (FFT skips MUSIC; the eigen modes keep computing the ratio but suppress the pseudospectrum/peak/stamp update, with ~1 dB hysteresis so a ratio hovering at the threshold can't flutter the display; under beamforming MUSIC keeps computing and steering holds the last open bearing). The DoA output is also hard-frozen — all methods, squelch on or off — while heimdall is calibrating (noise source active + settle hold), unless `DOA_DEBUG:1`. |
| `DEC_SQUELCH_EIGEN:<id>:<v>` | Per-decimator eigenvalue-ratio threshold (linear, clamped `1..1e6` — strong signals reach ratios of 1e4–1e5). |

### FFT display
| Command | Meaning |
|---|---|
| `FFT_SIZE:<n>` | FFT size (power of 2, 1024–65536): the finest spacing a zoomed view can reach is 2.4 MHz / n. |
| `FFT_VIEW:<display>:<seq>:<lo Hz>:<hi Hz>:<points>` | Ask for your own spectrum frames (type 7, §5.2) instead of type 0 / 6: `display` 0 = the main spectrum, 1 + ch = tuner ch in independent mode; `lo` / `hi` = the window in Hz from that display's centre (`lo >= hi` = the whole band); `points` = how many you want (your plot's width in pixels; 0 = 2048; at most 4096 are sent). `seq` is echoed in the frames. Per connection, not echoed or saved; the first one switches the connection to type 7. **The bandwidth lever:** fewer points = fewer bytes. |
| `FFT_DECIMATION:<n>` | Removed (was the "Downsampling" setting). Accepted and ignored - use `FFT_VIEW`'s points. |
| `BIN_DOA:<0\|1>` | Bearing spectrum ("Bearing colours"): the receiver estimates a bearing for every point of the main spectrum view (type 9, §5.2b). Saved, echoed and replayed like other settings; off by default. Costs receiver CPU while on and a connection takes it. |
| `BIN_DOA_VIEW:<0\|1>` | This connection takes type 9 frames (sent just before each main type 7 frame while `BIN_DOA` is on). Per connection, not echoed or saved; answered with `{"bin_doa":{...}}` (§6). An older receiver doesn't answer (it echoes it as a `sync_cmd`). |
| `GET_FFT_SETTINGS` | Query → replies `{"fft_settings":{"fft_size":n}}`. |

### Wideband / scanner
| Command | Meaning |
|---|---|
| `WIDEBAND_MODE:<0\|1>` / `WIDEBAND_BASE_FREQ:<MHz>` | Wideband enable / base freq. |
| `SCANNER_LOAD_CONFIG:<...>` / `SCANNER_GET_CONFIG` | Scanner config load / query. |
| `SCANNER_START` / `SCANNER_STOP` / `SCANNER_NEXT` | Discrete scanner control. |
| `SCANNER_SET_SQUELCH:<v>` / `SCANNER_SET_DWELL:<ms>` | Discrete scanner params. |
| `CONTINUOUS_SCANNER_START` / `CONTINUOUS_SCANNER_STOP` / `CONTINUOUS_SCANNER_STATUS` | Continuous scanner control. |
| `CONTINUOUS_SCANNER_SQUELCH:<v>` / `CONTINUOUS_SCANNER_DWELL:<ms>` / `CONTINUOUS_SCANNER_DECAY:<s>` | Continuous scanner params. |
| `CONTINUOUS_SCANNER_WIDEBAND:<0\|1>` / `CONTINUOUS_SCANNER_WIDEBAND_RANGE:<start>:<end>` | Continuous scanner wideband. |

### Browser-only display prefs (`UI:`)
`UI:WF_COLORMAP`, `UI:WF_MIN`, `UI:WF_MAX`, `UI:WF_SPEED`, `UI:WF_AUTO`,
`UI:FFT_Y` are **client-side display preferences**. The backend does not
interpret them — it only relays them to other browsers for display sync. A
native app renders its own display and does not need these; you may send them to
keep open browsers in sync, but they have no backend effect.

---

## 5. Stream frames (binary, received down)

All binary frames are **little-endian**. The first `uint32` is the message
**type**. Frames are self-delimited by the WebSocket framing itself (one logical
message per WS binary frame).

### 5.1 FFT — type `0` (topic `fft`, ~20/s)
Sent to connections that never sent `FFT_VIEW` (prefer type 7, §5.2: ~12x
fewer bytes). 2048 points per tuner band (FFT size / 2048 bins each).
Header, then arrays:

| Field | Type | Notes |
|---|---|---|
| type | uint32 | `0` |
| num_channels | uint32 | active channel count |
| decimation_factor | uint32 | RF decimation in effect |
| center_freq | float32 | MHz |
| gain | float32 | dB |
| averaging_alpha | float32 | 0..1 |
| channel_marker | uint32 | `0x80\|ch` = compressed single channel; `ch` = uncompressed; `0xFE` = wideband compressed; `0xFF` = wideband uncompressed |
| num_bins | uint32 | may be `0` when no data — then no arrays follow |
| frequencies | float32 × num_bins | per-bin center frequency (MHz) |
| min_envelope | uint8 × num_bins | compressed magnitude (min of bin group) |
| max_envelope | uint8 × num_bins | compressed magnitude (max of bin group) |

**Magnitude decompression:** `dB = value * (120.0 / 255.0)` (the encoder maps
dB range `[0,120]` → `uint8 [0,255]`). Each bin carries both the min and max
magnitude within its decimation group (draw as a vertical span).

### 5.2 Spectrum view — type `7` (~20/s per display, after `FFT_VIEW`)
One display's window (`FFT_VIEW`), max-pooled from the full-resolution FFT to
about the points asked for (each point = the max of its bins, so narrow peaks
survive): zoomed in, the same points cover less spectrum, down to one FFT bin
each. Independent mode: one frame per tuner (display 1 + ch). 64-byte header:

| Field | Type | Notes |
|---|---|---|
| type | uint32 | `7` |
| display | uint8 | 0 = main, 1 + ch = tuner ch |
| marker | uint8 | display 0: `0x80\|ch` coherent, `0xFE` wideband scan; else ch |
| channels | uint8 | active channel count |
| flags | uint8 | bit 0 = the frame is the requested window (else the whole band) |
| seq | uint32 | the `FFT_VIEW` it answers |
| center | float64 | display centre, Hz |
| band_lo / band_hi | float64 × 2 | first / last bin of the whole spectrum, Hz |
| start | float64 | frequency of point 0, Hz |
| step | float64 | point spacing, Hz (point i at start + i × step) |
| gain | float32 | dB |
| averaging_alpha | float32 | 0..1 |
| n | uint32 | points |
| levels | uint8 × n | `dB = value * (120.0 / 255.0)`, noise floor = 0 dB |

Frames are permessage-deflate compressed when the client offers it (most
WebSocket libraries do). A connection whose send buffer holds > 256 KB skips
frames instead of queueing them.

### 5.2a Beamformed FFT — type `4` (topic `fft`, or with the type 7 frames)
One per VFO, only while beamforming is enabled. Point spacing follows the
connection's main type 7 frame (zoomed in = finer), else that of type 0.

| Field | Type | Notes |
|---|---|---|
| type | uint32 | `4` |
| decimator_id | uint32 | the VFO |
| center_freq | float32 | MHz |
| bandwidth | float32 | MHz |
| n | uint32 | points, evenly over the band: point i at center - bw/2 + i × bw / n |
| steering_angle | float32 | degrees |
| snr_boost | float32 | dB |
| levels | uint8 × n | as type 7 |

### 5.2b Bearing spectrum — type `9` (with the main type 7 frame, after `BIN_DOA_VIEW:1`)
Coherent mode only, while `BIN_DOA:1` and not paused (calibration, retune,
stale calibration - the `bin_doa` state says why). Sent just before the main
display's type 7 frame and covering the same points (same `seq`, `start`,
`step`, `n`): point j's bearing, from the summed cross-spectral covariances
of the FFT bins behind it. 32-byte header:

| Field | Type | Notes |
|---|---|---|
| type | uint32 | `9` |
| display | uint8 | `0` (the main spectrum) |
| flags | uint8 | bit 0 = a ULA without a half-plane setting: the front half assumed; bit 1 = a 3D custom array: bearings at the horizon; bit 2 = every point's azimuth is its bearing (confidence 0 too; always set by this receiver) |
| elements | uint8 | antennas |
| topology | uint8 | 0 UCA (also the Wideband topology), 1 ULA, 2 custom |
| seq | uint32 | the `FFT_VIEW` it answers (as the type 7 frame) |
| n | uint32 | points |
| start | float64 | frequency of point 0, Hz |
| step | float64 | point spacing, Hz |
| confidence | uint8 × n | 0..255 = 0..1, informational: how far the point's coherent part stands above noise. The page does NOT use it - it colours every point at full strength (the user's choice); a client may use it to dim noise |
| azimuth | uint16 × n | × 360 / 65536 degrees: MUSIC's azimuth (counter-clockwise from ANT0, the array offset added - type 3's convention; compass = 360 − azimuth); every point has one (flag bit 2) |

Match points by frequency (`start + j × step`), not by index, if you redraw
a frame for another zoom.

### 5.3 Audio — type `1` (topic `audio`, ~25/s)
| Field | Type | Notes |
|---|---|---|
| type | uint32 | `1` |
| sample_count | uint32 | PCM samples this packet represents (1920) |
| sample_rate | float32 | 48000 |
| opus_size | uint32 | bytes of Opus payload |
| opus_data | uint8 × opus_size | Opus packet — mono, 48 kHz, 40 ms frames |

Decode with any Opus decoder (the browser uses `opus-decoder`). VBR ~32 kbps.

### 5.4 DoA — type `3` (topic `doa`, ~5/s)
| Field | Type | Notes |
|---|---|---|
| type | uint32 | `3` |
| decimator_count | uint32 | number of blocks following |

Then, repeated `decimator_count` times:

| Field | Type | Notes |
|---|---|---|
| id | uint32 | decimator id |
| freq_offset_khz | float32 | |
| bandwidth_index | uint32 | index into `BANDWIDTH_OPTIONS` |
| num_angles | uint32 | e.g. 360 |
| array_radius | float32 | |
| blocks_processed | uint32 | cumulative |
| resolution | float32 | degrees/bin (fixed at 1.0 — the grid is always 360 bins; sub-degree bearings come from peak interpolation) |
| pseudospectrum | float32 × num_angles | **linear** magnitude per azimuth bin |
| is_3d | uint32 | 0 = 2D (azimuth only), 1 = 3D |
| *(if is_3d=1)* num_elevation | uint32 | |
| *(if is_3d=1)* elev_resolution | float32 | degrees |
| *(if is_3d=1)* peak_elevation | int32 | degrees |
| *(if is_3d=1)* elev_spectrum | float32 × num_elevation | linear magnitude |

Peak azimuth = argmax of `pseudospectrum`; map to your display convention (the
browser/Android compass page uses `(360 - bearing) % 360`). For a sub-grid
readout, refine the argmax with a 3-point parabolic fit on
`ln(pseudospectrum)` around the peak (circular indexing; skip if a neighbor
bin is `0.0` from ULA masking) — this is what the backend does for the bearing
it logs and serves on `DOA_value.html` (one-decimal degrees).

The `pseudospectrum` already reflects `MUSIC_SIGNAL_SOURCES`, `ULA_MODE`, and
`ARRAY_OFFSET`: the array offset is baked into the bin angles (bin `i` →
`i * resolution` degrees in world frame), and ULA forward/backward truncation
zeroes the masked half-plane (those bins arrive as `0.0`). No client-side
correction for these settings is needed.

---

## 6. Control/status messages (JSON text, received down)

Delivered on the always-on control channel:

- `{"auth_required":true}` — send `AUTH:` (handshake).
- `{"auth_error":"..."}` — bad token; socket closing.
- `{"subscribed":{"fft":..,"audio":..,"doa":..}}` — `SUBSCRIBE` ack.
- `{"sync_cmd":"<command>"}` — another client (or this one) applied a command;
  re-apply to your UI to stay in sync.
- `{"scanner_sync":{...}}`, `{"doa_state":{"enabled":bool}}` — state pushes.
- `{"system_status":{...}}` (~2/s) — `hw` (cpu_temp, cpu_pct, ram), squelch
  state (`decimator_squelch[]` with per-decimator `id`, `open`, `est_sources` and
  `power_dbfs` — the VFO passband's FFT peak in dBFS, clamped to [-100, 0], the
  same value as field 4 of `DOA_value.html`; the "power" for a client's signal
  strength readout. `eigen_ratio` / `eigen_peak` / `eigen_thr` were dropped
  2026-10-10 with the eigenvalue squelch methods), `beamforming_status`, phase
  state, etc.
- `{"decimator_info":[...]}` — reply to `GET_DECIMATOR_INFO`.
- `{"bin_doa":{"on":bool,"state":"off|idle|running|parked","reason":"..","fft":n,"elements":m,"coverage":pct}}`
  — the bearing spectrum (`BIN_DOA`): `parked` = paused (reason: not coherent
  mode, calibrating, settling after a retune, calibration stale, array not set
  up), `idle` = nothing to do (no connection takes type 9, no data), `fft` =
  its FFT size, `coverage` = % of the samples used (below 100 when its CPU
  limit is reached). Sent on every change and in answer to `BIN_DOA_VIEW`.

---

## 7. Quick test (websocat)

```bash
# Fail-open (no token): stream control-plane only, then enable DoA
websocat --insecure wss://<host>:8080/ws <<<'SUBSCRIBE:DOA'

# With a token:
printf 'AUTH:%s\nSUBSCRIBE:FFT,DOA\nDOA:1\n' "$TOKEN" \
  | websocat --insecure wss://<host>:8080/ws
```

(`--insecure` skips the certificate check for testing — a real app pins the
server key or the receiver's CA, section 1.)

---

## 8. Trunk following

Commands `TRUNK_FOLLOW:0|1`, `TRUNK_MAX_VFOS:n`, `TRUNK_ENCRYPTED:0|1`,
`TRUNK_UNIT:0|1`, `TRUNK_AUDIO:0|1`, `TRUNK_HANG:s`, `TRUNK_ALL_SITES:0|1`,
`TRUNK_RETUNE:0|1`, `TRUNK_RETUNE_CAL:0|1`, `TRUNK_RETUNE_MAX:s`, `TRUNK_RETURN`,
`TRUNK_TUNERS:<list>`, `TRUNK_SEARCH:{..}`, `TRUNK_SEARCH_STOP`, `GET_TRUNK_CC`
(-> `{"trunk_cc":{...}}`), `TRUNK_CC_NAME:` / `_DEL:` / `_SET:` / `_FOLLOW:{..}`,
`TRUNK_CC_CLEAR`,
`TRUNK_END:id`, `TRUNK_HOLD:{..}`, the system edits `TRUNK_SYS_SET:` /
`TRUNK_TG_SET:` / `TRUNK_CH_SET:` / `TRUNK_RULE_SET:` (+ `_DEL`), `GET_TRUNK`,
`GET_TRUNK_SYS:<id>`; replies `{"trunk":{...}}` (status), `{"trunk_sys":{...}}`
(a system's lists), `{"trunk_error":{"cmd","error"}}`. A voice VFO carries
`"trunk":{"role":"voice",..}` in `decimator_info` (a control VFO
`"role":"control"`); its live state is in the status' `vfos`. Exact syntax
and every field: `docs/dev/trunk-following.md`.
