# DoA Client ↔ Heimdall v2 Consumption Reference

All paths relative to `/home/krakenrf/krakensdr_v2/kraken_doa_v2/`.

---

## 1. Data connection (port 8091)

### 1.1 Connection sequence
- Constant: `TCP_DATA_PORT = 8091` (`include/config.hpp:9`), host default `127.0.0.1` (`include/networking/tcp_client.hpp:14`).
- `DataReceiver::data_receiver_thread()` (`src/networking/data_receiver.cpp:79`) loops: `data_client.connect_to_port(TCP_DATA_PORT)` → on failure sleep **1 s** and retry (`data_receiver.cpp:105-109`).
- **The client sends NOTHING on the data socket — ever.** Plain `connect()` then read-only. No handshake bytes, no subscription message. Heimdall streams to every connected data client unconditionally.
- On successful connect (`data_receiver.cpp:112-122`):
  - First successful connect only: `ControlHandler::apply_persisted_settings()` replays saved settings (which forwards tuner commands over the control port). Guarded by `settings_restored` so reconnects do NOT replay (`data_receiver.cpp:96-99, 114-117`).
  - `bytes_in_buffer = 0` — any partial packet from a previous connection is discarded so old/new bytes never splice (`data_receiver.cpp:119-122`).

### 1.2 Socket options (TCPClient, `src/networking/tcp_client.cpp`)
- `SO_RCVTIMEO = 500 ms` set on every connection (`tcp_client.cpp:32-37`) — blocking `recv()` is bounded so the `running` flag is polled; timeout (`EAGAIN`/`EWOULDBLOCK`/`EINTR`) is treated as "no data yet", not disconnect (`data_receiver.cpp:130-134`).
- **No** `TCP_NODELAY`, **no** `SO_KEEPALIVE`, **no** send/receive buffer tuning, **no** application-level keepalive anywhere in the client (verified by grep; the only `SO_RCVBUF` in the tree is in `gpsd_client.cpp:176`, unrelated).
- `send_data()` uses `MSG_NOSIGNAL`, loops on short writes/`EINTR`, and on `EAGAIN` polls `POLLOUT` with **200 ms** timeout before retrying; any other failure → `disconnect()` (`tcp_client.cpp:51-82`).
- The **data** socket stays blocking (with the 500 ms rcv timeout). Only the **control** socket is set non-blocking (`data_receiver.cpp:1070`).

### 1.3 Receive buffer & packet parsing state machine
- Userspace accumulation buffer: `vector<uint8_t> buffer(4 * 1024 * 1024)` — **4 MiB** (`data_receiver.cpp:86`).
- `recv()` appends at `buffer.data() + bytes_in_buffer`; `bytes_read <= 0` (and not EAGAIN/EINTR) → `"TCP connection lost, reconnecting..."` to stderr, `disconnect()`, break to outer reconnect loop (`data_receiver.cpp:136-140`). Reconnect is immediate (the 1 s backoff only applies to failed `connect()`s).
- Parse loop (`data_receiver.cpp:146-417`): `while (offset + 32 <= bytes_in_buffer)`:
  1. Read `magic = read_be32(buffer, offset)`; if `magic != TCP_MAGIC` → `offset++` and rescan (**byte-by-byte resync on magic**, `data_receiver.cpp:148-153`).
  2. Decode the 7 remaining header words (see §1.4).
  3. **Sanity gate before acting** (`data_receiver.cpp:163-173`): reject `channels == 0 || channels > MAX_CHANNELS(5)` or `samples == 0 || samples > 65536` (`MAX_SAMPLES_PER_CHANNEL`, `data_receiver.cpp:90`) → treated as a **false magic match**: `offset++`, resync. This prevents huge allocations / parser stalls on a desynced stream.
  4. `header_size = 32 + channels*8`; `packet_size = header_size + channels*samples*2`. If `offset + packet_size > bytes_in_buffer` → `break` (wait for more bytes) (`data_receiver.cpp:254-259`).
  5. Build `RawDataPacket` (`include/utils/raw_data_buffer.hpp:14-61`), convert selected channels (§2), push into `raw_data_buffer`.
  6. `offset += packet_size`.
- After the loop: `memmove` the残 remainder to the front, `bytes_in_buffer -= offset` (`data_receiver.cpp:419-424`).
- Downstream queue: global `RawDataBuffer raw_data_buffer(150, 2048, milliseconds(10000))` — max **150 packets / 2048 MB / 10 s** age (`data_receiver.cpp:38`); consumer drops packets stale by > **1500 ms** (`data_receiver.cpp:451`).

### 1.4 Header wire format & endianness (CRITICAL — mixed!)
Fixed 32-byte header, then per-channel metadata, then payload:

| Offset | Field | Decode | Endianness |
|---|---|---|---|
| +0 | magic | `read_be32` | **Big-endian** `TCP_MAGIC = 0x4D434851` = ASCII `"MCHQ"` in wire order (`config.hpp:11`) |
| +4 | num_channels | `read_be32` | Big-endian |
| +8 | samples_per_channel | `read_be32` | Big-endian |
| +12 | phase_comp_state | `read_be32` | Big-endian |
| +16 | noise_source (0/1) | `read_be32` | Big-endian |
| +20 | frequency_change_counter | `read_be32` | Big-endian |
| +24 | current_group_index | `read_be32` | Big-endian |
| +28 | retuning_in_progress | `read_be32` | Big-endian |
| +32 + ch·8 | channel ch frequency_hz | `read_le_float` | **Little-endian float32** |
| +36 + ch·8 | channel ch gain_db | `read_le_float` | **Little-endian float32** |
| +32 + channels·8 | IQ payload | raw bytes | channels × samples × 2 uint8, **channel-major**: channel `ch` at `header_size + ch*samples*2`, interleaved I,Q (`data_receiver.cpp:386`) |

(`data_receiver.cpp:148-161, 252-287`; decode helpers in `include/utils/endian_utils.hpp`: `read_be32` = `memcpy` + unconditional `__builtin_bswap32` — i.e. assumes little-endian host; `read_le_float` = raw `memcpy`, host/LE float.)

`phase_comp_state`: the low byte is the `PhaseCompensatorState`; the higher
bits are flags every consumer masks off - `0x100` KerberosSDR manual mode,
`0x200` calibration stale, `0x400` wideband scan, `0x800` independent mode,
`0x1000` generic RTL-SDRs (heimdall started without a KrakenSDR: no coherent
array, coherent mode unavailable; set on every frame together with `0x800`
or `0x400`), `0x2000` sweep mode (always together with `0x800`, so a client
that doesn't know sweep mode takes the stream for independent tuners; one
that does tests `0x2000` first), `0x4000` trial gain (sweep mode with Auto
gain, added 2026-11-24: some channel of this packet was taken at a trial gain -
a linearity probe, not the hop's own gain; bits 16 + ch, `0x10000 << ch`, say
which channels. The samples are valid and the per-channel gain is the one
they were taken with. Read with the per-channel frequency and gain, before
the retuning flag, so the guarantee below covers it. A client that keeps one
level per hop can leave such a visit out, or shift it by its gain like any other).

**Sweep mode** (`0x2000`, added 2026-11-20; the contract:
`kraken_doa_v2/docs/dev/sweep-mode.md`): the chosen tuners hop through the
plan set with `set_sweep` (control protocol §2.18). Per-channel frequency =
where that tuner is, per-channel gain = the gain that tuner's samples in
this packet were taken with - the common gain, or with Auto gain
(`set_sweep_auto_gain`, 2026-11-23) the gain of that hop, a value of the
tuner's gain list at or below the common gain. It is read with the
frequency, before the flag, so the guarantee below covers it too. The scanner fields
belong to the sweep: `frequency_change_counter` + 1 per round,
`current_group_index` = the round r the tuners are on, and
`retuning_in_progress` = 1 from before the tuners move until the settle and
the flush after it are done. The guarantee: a packet with
`retuning_in_progress` = 0 holds only samples taken at the header's
per-channel frequencies, after the settle (heimdall reads the per-channel
frequencies before the flag; the flag is cleared only after the post-settle
flush). The frequency is a float32 (~128 Hz coarse at 1.7 GHz): find the hop
as `round((f - first_center_hz) / step_hz)`. Tuners that take no part never
move. A per-channel frequency of **0** means "no valid frequency": that
tuner's last retune failed (the R820T / E4000 drivers fail after N / SDM are
written, so the tuner is NOT on its old frequency) - its samples are no hop's
data until its next successful retune puts a real frequency back (added
2026-11-20). In that generic mode `num_channels` can be **1** (a single
dongle); heimdall's own GR source (`krakensdr_source_impl.cc`) accepts 1-16.

Wire check (generic mode, one R820T dongle, 2026-11-16): `channels=1
samples=16384 phase_state=0x1800 noise=0`, 2.400 MS/s over 10 s.

Per-channel metadata is pushed into `ChannelManager::update_channel_info(ch, freq, gain)` for every packet (`data_receiver.cpp:286`), which also stores `tuner_frequencies[ch]` (uint64, `src/channel_manager.cpp:68-80`).

### 1.5 num_channels / num_samples changes mid-stream
- Both are re-read from **every packet header**; all vectors (`channel_iq_data`, metadata) are resized per packet. No stream-level renegotiation exists.
- Global `num_channels` atomic updated every packet (`data_receiver.cpp:406`).
- `active_num_elements` (used by MUSIC/beamformer/UI) is synced from the packet when `channels != current && channels >= 2 && channels <= DOA_NUM_ELEMENTS(5)` (`data_receiver.cpp:409-414`).
- Out-of-range values are indistinguishable from desync and cause byte-wise resync (§1.3 step 3).

### 1.6 Reconnect/backoff summary
- connect failure → 1 s sleep, retry forever while `running`.
- recv error/EOF → immediate disconnect + reconnect attempt (then 1 s backoff on further failures).
- No exponential backoff; parse buffer cleared on each fresh connection; `first_packet` counter latch re-initializes `last_frequency_change_counter` on the first packet only of the *process* lifetime (static-free local, `data_receiver.cpp:93-94, 244-248`).

---

## 2. uint8 IQ → complex<float> conversion

`IQConverter::convert_uint8_to_complex_float()` (`src/utils/iq_converter.cpp:9-54`), called at `data_receiver.cpp:389-394` with `dc_correction = true` **always** in the packet path:

- Constants: `off = 127.5f`, `scl = 1.0f / 127.5f` (`iq_converter.cpp:19-20`).
- **DC-corrected path (the one actually used)**: two passes —
  1. compute `mean_i`, `mean_q` over the block (sums of raw uint8 values / num_samples);
  2. `output[i] = { (float(input[2i]) - mean_i) * scl, (float(input[2i+1]) - mean_q) * scl }`.
  So the offset subtracted is the **measured per-block DC mean, not 127.5** — 127.5 only defines the scale. Removes the FFT center spike.
- Non-DC path (not used by the receiver): `(float(x) - 127.5f) * (1/127.5f)` → range ≈ [-1.0, +1.0]. **Uses 127.5, not 128.**
- Plain scalar loops, compiler auto-vectorized (`-O3 -march=native`); intentionally NOT NEON intrinsics.
- Only channels that are actually needed are converted (stack flag array `channels_to_convert[MAX_CHANNELS]`): all channels in wideband-scan mode; channels 0..DOA_NUM_ELEMENTS-1 if DoA/beamforming/FM-with-5ch; plus the active FFT/FM channel. OpenMP `parallel for` if > 2 channels (`data_receiver.cpp:323-397`).

---

## 3. Control connection (port 8092)

### 3.1 Transport
- `TCP_CONTROL_PORT = 8092` (`config.hpp:10`). Single shared `TCPClient control_client` (`src/main.cpp:75`).
- `DataReceiver::send_control_command()` (`data_receiver.cpp:1048-1076`):
  - Serialized by a static `control_send_mutex` (uWS thread + scanner thread both send).
  - **Newline-framed**: `cmd + "\n"` — heimdall splits commands on newlines; without it back-to-back commands coalesce (`data_receiver.cpp:1056-1060`).
  - **Lazy connect**: connection opened on first command; socket set `O_NONBLOCK` after connect (`data_receiver.cpp:1067-1070`).
  - **Two attempts** then drop with stderr `"Control command dropped (server unreachable): ..."`.
- The client drains the control socket on its status thread (`drain_control_socket`) and takes only a few things from heimdall's status lines: the Wideband board's `downconverter` object, heimdall's `version` and, since 2026-11-18, the antenna bias tee fields and the replies to `set_antenna_bias_tees` (`kraken_doa_v2/docs/dev/bias-tees.md`; for those it also opens the control connection itself, every 5 s at most while it is down). All other status (phase_state, noise_source, frequencies, gains, retuning) is consumed from the **data-packet header**.

### 3.2 Exact JSON commands sent to heimdall
All single-line JSON + `\n`:

| Trigger (browser WS cmd) | JSON sent to 8092 | Ref |
|---|---|---|
| `FREQ:<MHz>` | `{"set_frequency":{"frequency":<hz_uint64>,"channel":<active_ch>}}` | `control_handler.cpp:295-297` |
| `WIDEBAND_FREQ:<MHz>` / scanner hops / continuous scanner retunes | `{"set_frequency":{"frequency":<hz>}}` (no channel key) | `control_handler.cpp:429-431`, `scanner_manager.cpp:677`, `continuous_scanner.cpp:726` |
| `GAIN:<dB>` | `{"set_gain":{"gain":<float_db>,"channel":<active_ch>}}` | `control_handler.cpp:454-456` |
| `FREQ_OFFSET:<kHz>` | `{"set_freq_offset":{"offset_hz":<hz_float>}}` | `control_handler.cpp:439-441` |
| `MIXER_SIDE:high\|low\|below` (wb variant; also re-asserted after every FREQ) | `{"set_mixer_side":{"side":"high"\|"low"\|"below"}}` | `control_handler.cpp:339-341, 385-387` |
| `LO_CURRENT:0..7` | `{"set_lo_current":{"current":<0-7>}}` | `control_handler.cpp:414-416` |
| `WIDEBAND_MODE:0/1` | `{"set_wideband_mode":{"enable":true\|false}}` | `control_handler.cpp:1179-1181, 280` |
| `WIDEBAND_BASE_FREQ:<MHz>` | `{"set_wideband_frequencies":{"base_frequency":<hz_uint32>}}` | `control_handler.cpp:1193-1196` |
| `EDGE_CLIP:<0.1-1.0>` | `{"set_wideband_edge_clip":{"edge_clip":<f>}}` | `control_handler.cpp:1430-1432` |
| Discrete scanner config | `{"configure_scanner":true,"frequency_groups":[<hz>,...],"dwell_time_ms":<ms>}` | `scanner_manager.cpp:187-196, 316-326` |
| Scanner start/stop | `{"start_scanner":true}` / `{"stop_scanner":true}` | `scanner_manager.cpp:333-334, 367-369, 651-653` |
| Scanner lock → coherent | `{"set_wideband_mode":{"enable":false}}` then `{"set_frequency":...}` | `scanner_manager.cpp:663-678` |
| Scanner resume | `{"set_wideband_mode":{"enable":true,"base_frequency":<hz>}}` + re-config + start | `scanner_manager.cpp:820-863` |
| Continuous scanner start/stop | `{"set_stability_delay":{"delay_ms":500}}` on start (wideband scan), `{"set_stability_delay":{"delay_ms":3000}}` restored on stop | `continuous_scanner.cpp:59-61, 137-139` |

`CHANNEL:`, `AVG:`, `BANDWIDTH:`, all MUSIC/topology/squelch/beamforming commands are **client-local** — nothing is sent to heimdall for them.

### 3.3 Phase-state / noise-source gating of DoA (verified logic)
- Every consumed packet calls `scanner_manager.updatePhaseCalibrationStatus(phase_comp_state, noise_source_active)` (`data_receiver.cpp:458-459` → `scanner_manager.cpp:557-576`): stores both atomics; if `noise_source_active` it stamps `last_noise_active_ns_` (steady_clock ns) so pollers catch bursts between polls; when scanner state is LOCKED it merely *logs* on `phase_state == 4` ("Phase calibration converged").
- **The DoA gate is `doa_is_calibrating()` (`src/doa_logger.cpp:37-51`) and it gates ONLY on the noise source, never on phase_state**:
  ```cpp
  constexpr long NOISE_QUIET_MS = 750;   // hold after last noise-on packet
  return scanner_manager.isNoiseSourceActive() || recent_noise;  // recent = within 750 ms
  ```
  Rationale in-code: steady-state DoA reports **phase_state 4 (CONVERGED)** — heimdall enum `heimdall_v2/src/core/types.hpp:28-30`: `WAITING_FOR_LAG_COMPLETION=0, MEASURING_INITIAL_PHASE=1, APPLYING_COMPENSATION=2, VERIFYING_CONVERGENCE=3, CONVERGED=4` — so a `phase_state < N` freeze would wrongly freeze everything. (Matches the memory note.)
- Where the gate is applied:
  - MUSIC pipeline hard gate: MUSIC is skipped entirely while `doa_is_calibrating() || doa_retune_hold_active()` (`data_receiver.cpp:593-600`) → published DoA freezes and covariance/auto-squelch never see noise-source data.
  - Auto eigenvalue-squelch learning: `allow_learn = fft_quiet && !doa_is_calibrating()` (`data_receiver.cpp:673`).
  - `DOA_value.html` HTTP endpoint: serves `last_good_message` verbatim while calibrating (`src/networking/websocket_server.cpp:493-505`).
  - DoA logger: `if (doa_is_calibrating()) continue;` (`doa_logger.cpp:622`).
- Cosmetic bug worth knowing: the receiver's log-name table `{"WAITING_LAG","MEASURING","WAITING_STABILITY","APPLYING","VERIFYING","CONVERGED"}` (`data_receiver.cpp:178`) has **6 entries and is misaligned** with heimdall's 5-state enum (would print "VERIFYING" for CONVERGED=4). Logging only; no logic depends on it.
- `noise_source` also surfaces to browsers as `"noise_source":true/false` in the 500 ms system_status JSON (`src/message_builders.cpp:447`).

---

## 4. Use of freq_change_counter / retuning_in_progress / current_group_index

All three come from the packet header (§1.4). Central mechanism: `g_last_retune_complete_ms` atomic + `RETUNE_DOA_HOLD_MS = 4000` ms hold during which **all** MUSIC processing is skipped (`data_receiver.cpp:61-76`) — needed because heimdall's ~3 s post-retune cooldown streams settling data with the noise source OFF, invisible to `doa_is_calibrating()`.

- **`retuning_in_progress`** (`data_receiver.cpp:186-229, 311-321`):
  - While nonzero: packet is **not processed** (no conversion, no FFT, no DoA); only `tuner_frequencies[ch]` are updated so the UI shows the target frequency; `offset += packet_size; continue`.
  - Falling edge 1→0 ("retuning complete"): arms the 4 s DoA hold; in non-wideband mode also bumps `fft_reset_generation`, sets `fft_data_valid=false`, drains `fft_work_queue`, `FFTProcessor::reset_all_buffers()`, clears wideband FFT buffers to -100 dB. In wideband-scan mode FFT reset is skipped (signals stay visible during drag-to-tune).
  - Known caveat (documented in code): the pulse can fall entirely between packets (~10-50 ms tuner reprogram), hence the two backup triggers below.
- **`frequency_change_counter`** (`data_receiver.cpp:231-248`): any change (after the first packet) logs `"Server frequency change detected!"` and arms the 4 s DoA hold; counter latched. Bumped by scanner group changes on the server.
- **Reference-channel metadata frequency** (belt-and-braces, `data_receiver.cpp:289-309`): if channel 0's header float changes by > 1.0 Hz between packets, arm the 4 s hold. Described as the only "ground truth in every packet" for plain retunes.
- **`current_group_index`**: decoded, logged in the frequency-change message, and stored in `RawDataPacket.current_group_index` (`data_receiver.cpp:160, 240, 267`) — **no consumer reads it downstream** (grep-verified: only set/logged). Discrete-scanner group tracking is done client-side instead.

---

## 5. Keepalive / socket options / buffer sizes (consolidated)

| Item | Value | Ref |
|---|---|---|
| Data/control connect host | `127.0.0.1` default | `tcp_client.hpp:14` |
| `SO_RCVTIMEO` | 500 ms (both sockets) | `tcp_client.cpp:35-37` |
| `TCP_NODELAY` / `SO_KEEPALIVE` / app keepalive | **none** | grep-verified |
| Control socket | non-blocking after connect; send retries with 200 ms POLLOUT wait; `MSG_NOSIGNAL` | `data_receiver.cpp:1070`, `tcp_client.cpp:62-77` |
| Data receive accumulation buffer | 4 MiB userspace | `data_receiver.cpp:86` |
| Max samples/channel accepted | 65536 | `data_receiver.cpp:90` |
| `MAX_CHANNELS` | 5 | `config.hpp:23` |
| `RawDataBuffer` | 150 packets / 2048 MB / 10 s max age | `data_receiver.cpp:38` |
| Consumer staleness drops | packet > 1500 ms; FFT work item > 500 ms; FM item > 200 ms | `data_receiver.cpp:451, 917, 982` |
| FFT work queue soft caps | 50 (normal) / 100 (wideband) items | `data_receiver.cpp:875, 832` |
| FM decimated queue cap | 20 items | `data_receiver.cpp:786` |
| Connect retry backoff (data) | fixed 1 s | `data_receiver.cpp:108` |
| Control command framing | JSON + `"\n"`, 2 attempts, then dropped with stderr log | `data_receiver.cpp:1060-1075` |
| Settings replay | once, on first data-port connect, via `ControlHandler::apply_persisted_settings()` → full command path incl. server forwards | `data_receiver.cpp:114-117`, `control_handler.cpp:1530-1541` |

Key files: `src/networking/tcp_client.cpp`, `src/networking/data_receiver.cpp`, `src/control_handler.cpp`, `src/utils/iq_converter.cpp`, `src/doa_logger.cpp:37`, `src/scanner_manager.cpp:557`, `include/utils/endian_utils.hpp`, `include/utils/raw_data_buffer.hpp`, `include/config.hpp`, `include/globals.hpp`.