# Web Mapper output

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Web Mapper output (built-in, replaces web_mapper_middleware):**
- `src/networking/web_mapper.cpp` streams one legacy "doapost" record per VFO
  to the KrakenSDR web mapper — the record is built from the SAME capture
  helpers as DOA_value.html / the DoA logger (`capture_doa_records()`), so it
  stays byte-compatible with the external contract with map.krakenrf.com and
  the Android app. The old Node.js `web_mapper_middleware/` is deprecated and
  no longer needed (no Node/npm install, nothing extra to run)
- Two modes: `remote` (WSS client to the KrakenPro cloud map; hand-rolled
  TLS WebSocket client over OpenSSL — uWS is server-only) and `local` (plain
  WebSocket broadcast server, default port 8021, for LAN map clients). The
  local-mode HTTP settings endpoint the middleware had (port 8042) is gone:
  config lives in the sidebar now
- Remote mode also REGISTERS the station: the legacy flat settings.json
  schema is pushed on connect/change (1 s hash-guarded check) with a ping
  every 10 s, and settings pushed BACK by the cloud are applied to the
  receiver by dispatching normal control commands on the uWS loop
  (`loop->defer` → `ControlHandler::handle_websocket_message`), so sync,
  persistence and heimdall forwarding behave as if a browser sent them.
  Cloud VFO bandwidths are applied in Hz as they are (`SET_DECIMATOR_BW_HZ`;
  they snapped to the nearest `BANDWIDTH_OPTIONS` entry until 2026-10-21);
  `vfo_squelch_N` sets the level AND enables that VFO's squelch (the mapper
  assumes squelch is always on)
- Everything runs on one worker thread started from main(); records are
  captured on a 200 ms tick, gated on `doa_enabled` and
  `doa_is_calibrating()` (noise-source pulses would map garbage bearings).
  Each VFO's own squelch setting is respected (no separate mapper setting):
  squelch off = that VFO's bearings always transmit; squelch on = only
  while its squelch is open
- WS commands (persisted via settings_store, replayed to new browsers):
  `WEB_MAPPER:0|1`, `WEB_MAPPER_MODE:remote|local`, `WEB_MAPPER_KEY:<key>`,
  `WEB_MAPPER_URL:wss://...`, `WEB_MAPPER_WS_PORT:<port>`. The key is
  SECRET: it is persisted but never echoed or replayed to browsers -
  `redact_for_sync()` (control_handler.cpp) turns it into
  `WEB_MAPPER_KEY_SET:1|0`, and the UI field stays empty with a "saved"
  placeholder. Live state rides system_status as
  `web_mapper:{enabled,mode,state,clients,records,error}`; the sidebar
  "🌐 Web Mapper" panel drives it all
- The cloud connection VERIFIES the server certificate (system CA store +
  hostname; map.krakenrf.com serves a valid `*.krakenrf.com` cert). The
  legacy middleware didn't, which let anyone on the network path pose as
  the map server and collect the API key. `KRAKEN_WEB_MAPPER_INSECURE=1`
  (environment) turns verification off for a self-hosted server with a
  self-signed certificate. A cloud settings push can NOT change the server
  URL (`mapping_server_url` is ignored) - that stays a local, web-UI-only
  decision. Reassembled cloud messages are capped at 1 MB
- Station identity/location are NOT web-mapper settings: the callsign and
  the resolved lat/lon/heading come from StationInfo (the "Station
  Information" panel), exactly like DOA_value.html

