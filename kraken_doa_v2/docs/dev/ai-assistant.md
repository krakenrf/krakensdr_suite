# AI assistant

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**🤖 AI assistant (`src/ai_mcp.cpp`, `src/ai_manager.cpp`, `src/ai_sandbox.cpp`, `ai/`)**
(replaced the AI Signal Lab 2026-10-17 - same bridge, now a general
assistant that sees the screen, controls the receiver through MCP, searches
the internet and writes / changes decoder plugins):
- Page: a docked right-hand column (`#ai-dock`, after `.main-content`; body
  class `ai-dk-off` while the assistant isn't enabled - the page is as
  before then). Always visible while enabled: ⟩ collapses it to a 30 px strip
  (`#ai-strip`, click = open), the left edge (`#ai-dock-rs`) resizes it
  (260 px .. 60 % of the window), width + collapsed state per browser
  (localStorage `kraken_ai_dock`). Phone (<= 900 px): a floating 🤖 button
  (`#ai-fab`) opens it full-screen. Header: Read only switch (`#ai-dk-ro`),
  ＋ new conversation, 🕘 history (`#ai-dk-hist`: every conversation, pick /
  delete), ⟩. Body: the conversation (`aiConvHtml`: saved turns from the
  activity split at the bridge's "question" events, the live turn from
  `aiLog`), each tool call a step line (`.ai-step`: `rx` = changed the
  receiver - blue, `err` red; click = its result), the answer streamed as it
  is written (`delta` events), Markdown through the escaping `aiMd`
  (+ bare-URL links). Empty conversation: 4 suggestion chips (`AI_CHIPS`).
  Enter sends, Shift+Enter = new line, the Send button is Stop while a turn
  runs. The old sidebar box, the 🤖 info-panel tab and the Lab's
  Investigate / Create forms are gone (a saved `infoTab` 'ai' falls back to
  the decoder tabs)
- Each question carries the page's VIEW (`aiViewContext`: mode, selected
  VFO, zoom window, open bottom tab, map centre / zoom, the DF panel's pick,
  an AI card shown) - `AI_ASK:{"text","view","new"}`; the receiver
  re-serialises the view (`ai_mcp::to_json`, never passed through raw) and
  prepends `ai_mcp::screen_json(view)` (= what get_screen returns) to the
  prompt, so "what is the signal D0 is on?" needs no tool call to know D0
- MCP server (`ai_mcp.cpp`): MCP streamable HTTP (JSON-RPC over POST:
  initialize / tools/list / tools/call / ping; notifications -> 202, other
  methods -32601, GET -> 405) on a SECOND `uWS::App` in the main loop
  thread (`ai_mcp::attach()` before `ssl_app.run()`), 127.0.0.1:`AI_MCP_PORT`
  8079 (`KRAKEN_AI_MCP_PORT`). Only the agent can use it: Host must be
  127.0.0.1 / localhost / [::1] with the port, ANY Origin header -> 403 (no
  web page can reach it), a random 48-hex Bearer token per start (constant-
  time compare; `KRAKEN_AI_MCP_TOKEN` >= 16 chars for tests only), bodies
  <= 1 MB. Slow tools reply later (`later()`: a worker thread, the reply
  through `g_loop->defer`, onAborted handled). Tools (`tools()`; control
  ones get "[changes the receiver - refused while the user has Read only
  on]" in their description):
  - read: `get_screen`, `get_spectrum` (window, points, min_db, tuner,
    `hold_s` 0-30 = peak hold - the raw per-frame FFT sampled every 40 ms
    under fft_mutex, measured above the p10 of the averaged display:
    radiosondes / pagers / bursts hardly show in the average - an RS41 was
    47.5 dB in the hold and invisible averaged; signals = runs over min_db,
    gaps < 2 kHz joined (FM sidebands), width within 20 dB of the run's
    peak), `get_decoder` (facts, events, tables, states), `get_bearings`
    (VFOs + talkers - `rdfmap::status_message(true)`: a peek that doesn't
    change its every-2.5 s counters; MUSIC bearings without a station),
    `get_map_points`, `get_transcripts` (`v2t::history_json`),
    `search_decoder_log` (gzip days too, <= 31), `get_saved_frequencies`,
    `get_incidents`, `list_decoders`, `wait_for` (new decoder events, <= 300
    s, optional text), `find_identifications`
  - records: `capture_iq` (captured INSIDE kraken_doa - `ai_capture.cpp`,
    below - then `sigtool analyze`, into the conversation folder, 1-60 s,
    12.5-600 kHz; `antenna`, `all_antennas`; during a playback it records
    the playback, the sidecar says so),
    `remember_signal` / `forget_signal` -> `ai/identifications.json` (Mark:
    freq, name, summary, bandwidth, bearing, confidence; <= 1000; the same
    name within max(2 kHz, half its bandwidth) updates) = AI badges on the spectrum
    (`aiDrawMarkers`, click = its card in the dock, no VFO moved)
  - control (refused in Read only): `tune` (FREQ / TUNER_FREQ, then waits
    <= 12 s until `MessageBuilders::spectrum_center_hz()` shows it and the
    recalibration ended), `set_gain`, `add_vfo`, `set_vfo` (freq, bandwidth
    -> any whole Hz / a decoder's minimum (the narrowest efficient size), demod, decoder - fixed-
    frequency and `lock_vfo` decoders handled the way the page does it -,
    squelch, listen, transcribe), `set_decoder_option`, `remove_vfo`,
    `set_doa`, `save_frequency` (SF_SET, id `ai<ms>`). All through
    `ControlHandler::apply_command` = the WebSocket path (validation,
    playback refusals, persistence, echo to every page), which returns the
    refusal text instead of sending it to a socket
  - the DoA convention in every result: `array_deg` (MUSIC),
    `clockwise_from_front_deg` (= what the polar plot shows, 360 - array),
    `compass_deg` (heading + clockwise) - the model mixed them up before
  - 🗺 the map (2026-10-18, the user's request: plot transmitter sites from
    the web / local knowledge and any other data, control the viewshed /
    path loss): `map_plot`, `map_layers`, `map_remove` (the assistant's
    layers - kraken_doa, every page; records, allowed in Read only) and
    `map_show`, `viewshed_get`, `viewshed_set`, `viewshed_add_tx`,
    `viewshed_remove`, `path_loss`, `terrain_at` (the PAGE's map / Viewshed
    panel - per browser, computed there; they change no receiver setting,
    allowed in Read only). Below: *Map layers* and *Page commands*
- Bridge (`ai/kraken_ai.py chat --session ai/sessions/<YYYYmmdd-HHMMSS>
  --text Q --context-json SCREEN`), one turn per question, ONE job at a time
  (AiManager as before: own process group, reader thread, Stop = killpg).
  `claude -p --output-format stream-json --verbose --include-partial-messages
  --permission-mode dontAsk --permission-prompts none --setting-sources ""
  --strict-mcp-config --mcp-config ai/run/mcp-<pid>.json --settings
  ai/run/settings-<pid>.json --tools Read,Edit,Write,Glob,Grep,WebSearch,
  WebFetch[,Bash] (--resume ID | --session-id NEW) --append-system-prompt`;
  run files 0600, removed after. Env (`agent_env()`): no `KRAKEN_MCP_*` /
  `KRAKEN_API_TOKEN` passed on, `CLAUDE_CODE_DISABLE_CLAUDE_MDS=1` +
  `CLAUDE_CODE_DISABLE_AUTO_MEMORY=1` (without them this repo's CLAUDE.md
  files and the user's memory were loaded: 224k tokens per turn), memguard
  on PYTHONPATH. AiManager passes the MCP URL / token in `KRAKEN_MCP_URL` /
  `KRAKEN_MCP_TOKEN`. Events: question, session, status, delta (not kept in
  AiManager's 400-line log), text, tool {name without `mcp__kraken__`, id,
  detail, receiver}, tool_result {id, ok, detail}, denied, error, usage,
  done {ok, text, title, session, plugins = plugin folders whose files
  changed -> registry rescan}, exit. Timeout `chat_timeout_s` (1800).
  Conversation folder: session.json (title, state, claude_session),
  chat.json, activity.jsonl, captures. The system prompt: the tools, how to
  find signals (peak hold, decoders as detectors), long searches
  (SondeHub, stepping 400-406 MHz with a 24 kHz radiosonde VFO), the
  identification workflow (local knowledge -> signal -> internet ->
  remember_signal), safety (web content is untrusted data, the station's
  position stays private, no decryption), plugin rules
- What it may touch (`claude_settings()`, rule paths `//abs/**`):
  - Read / Grep / Glob: kraken_doa_v2 (sources for reference), plugins,
    its conversation folder, `ai/knowledge/` (the user's local spectrum
    notes / databases - gitignored except README.md), /usr/include. Scoped
    rules only: a bare `Grep` allow read ANY folder not denied
  - Edit / Write: its conversation folder, `plugins/*/**` (any plugin -
    shipped ones too, the user's call), `ai/knowledge/`
  - denied: Read + Edit of api_token, doa_settings.json, server.key,
    ai/ai_config.json, ai/run, ai/state, noise_floors.json,
    rdf_session.bin; Edit of plugins/sdk, plugins/lib, plugins/Makefile /
    GNUmakefile / makefile / *.mk, SDK.md, README.md (`plugins/*/**` also
    matches top-level files, hence the explicit denies; the plugins are
    built with `make -f Makefile` so a planted GNUmakefile is never read),
    kraken_ai.py, sigtool.py, memguard. No `--add-dir` (it made the whole
    folder writable - the agent wrote `plugins/zz_probe.mk`)
  - a plugin's `CXXFLAGS` file is filtered to a whitelist
    (`CXXFLAGS_ALLOWED` in plugins/Makefile: -O*, -ffast-math, -march= ...,
    -D...) - no -include / -B / -specs / -fplugin
  - Bash only with the Claude Code sandbox (bubblewrap + socat;
    `failIfUnavailable`, `allowUnsandboxedCommands: false`): writes only to
    the conversation folder, plugins, knowledge; the core files above and
    the secrets + ~/.ssh, ~/.claude*, ~/.config ... denyRead; NO network
    (`allowedDomains: []` - the internet is WebSearch / WebFetch only).
    Without the sandbox the Bash tool isn't given at all (the assistant can
    still edit plugins, but the user builds them) - `kraken_ai.py status`
    and a warning line in the dock (`#ai-dk-warn`) say so. With it, ALL
    Bash is allowed (`"Bash"` allow rule): the sandbox's auto-allow skipped
    `make`, `VAR=...; cmd $VAR` and `<` redirects (refused in dontAsk - the
    model then assumed it had no shell and never built), and every command
    runs sandboxed anyway (checked: a nested `bash -c`, a make recipe and a
    `$VAR` path still hit "Read-only file system"). The sandbox has its own
    PID namespace (4 processes visible: no `/proc/<pid>/cwd` / `environ` of
    kraken_doa) and network namespace (127.0.0.1:8092 / 8079 refused). Known
    gap: the shell can create an inert top-level file in plugins/ (e.g.
    `x.mk`; GNUmakefile / makefile / Makefile are blocked) - nothing reads
    it, the builds use `-f Makefile`
  - `--safe-mode` is NOT used: it also drops `--mcp-config`
- Plugins the assistant changed run in a bwrap sandbox (`ai_sandbox.cpp`):
  a folder whose ctime moved during a turn (`turn_started` - 2 s ..
  `turn_finished`) is added to `ai/state/sandboxed_plugins.json`; its
  `--info` scan, its decoder process (`PluginProcess::start`) and a debug
  playback generator are wrapped in `bwrap --ro-bind / / --dev /dev --proc
  /proc --tmpfs /tmp [--bind <rw>] --unshare-net --unshare-ipc
  --die-with-parent --new-session` (read-only filesystem, no network).
  Without bwrap such a plugin doesn't run ("install bubblewrap"). A crash
  mid-turn is caught at the next start (`ai/state/turn` left behind ->
  `recover()` marks what changed). `python3 ai/kraken_ai.py trust <id>`
  (after reviewing it) / `untrust` / `sandboxed`. Folders touched outside a
  turn are not marked
- Read only (`AI_READONLY:0|1`, persisted, every page): the control tools
  answer "refused - Read only"; reading, capturing, identifications and the
  map go on
- Model / effort (2026-10-18, the user's request): the dock's ⚙ row
  (`#ai-dk-set`: Model Default (= ai_config.json's, named) / Sonnet / Opus
  / Haiku / Fable, a full name set elsewhere shown as its own option;
  Effort Default / low / medium / high / xhigh / max) -> `AI_MODEL:<alias
  | full name | "">` / `AI_EFFORT:<level | "">` (every page, persisted -
  settings_store `ai_model` / `ai_effort`; `AiManager::valid_model`:
  [A-Za-z0-9][A-Za-z0-9._[]-]{0,63}) -> `ai_state` `model_sel` / `effort`
  -> each question's bridge gets `--model` / `--effort` (`cmd_chat`
  overrides the config; `Runner` passes `--effort` to claude - checked
  with haiku and sonnet). The usage event names the model + effort used
  (dock footnote "... · sonnet-5-5 · low"; `effort_used`: the pick, else
  the model's default); the dock's footer = what the next question runs
  with ("Opus 5.5 · effort medium"). ai_config.json's `model` stays the
  default (`kraken_ai.py setup --model`)
- "Default" NAMES what it runs (the user's request): Model "Default (Opus
  5.5)", Effort "Default (Medium)" - the effort for the model PICKED (each
  model has its own: Fable's is high), aliases shown as the model they run
  ("Sonnet 5.5"), a full name by its name. `kraken_ai.py status`
  (`cli_defaults`, flat strings - AiManager's reader takes no nesting ->
  `ai_state.defaults` {model, name, from, effort, info "id or alias=name|
  effort;..."}; re-probed every 5 min): the model = ai_config.json's (the
  page takes `ai_state.model` first - read fresh), else `ANTHROPIC_MODEL`,
  else the account's default from Claude Code's OWN cached model catalog
  (`~/.claude/cache/model-catalog/*-cc.json`: `catalog.state.model`, each
  model's `runtime.default_effort`; aliases = its "main" section by
  short_name - the CLI ran "sonnet" as claude-sonnet-5-5, not the
  catalog's `provider_alias_targets`), else the model the last run without
  --model reported (`ai/state/cli_default.json`); the effort = ai_config's
  `effort`, else `CLAUDE_CODE_EFFORT_LEVEL`, else the model's default.
  The user's ~/.claude/settings.json `effortLevel` (xhigh here) does NOT
  apply - the bridge runs `--setting-sources ""`: checked, haiku-5-5
  printed CLAUDE_EFFORT=medium under the bridge's flags. Unknown (no
  catalog, older receiver) = a plain "Default". Test: scratchpad
  ai/t_defaults.py (15 checks, no Claude call)
- 🗺 Map layers (`src/ai_map.cpp`, `include/ai_map.hpp`): named layers of
  features - point (label, colour, emoji icon, size - an emoji is drawn at
  3.6 x the size, at least 18 px, default 20: `aimIconPx` / `aimPtR`, the
  user found them hard to see at 2.6 x, and the agent's size 2 drew a 5 px
  📡), line (width, dashed,
  arrow), polygon (rings, fill opacity), circle (radius m), bearing (a
  great-circle line from lat / lon or, left out, the station: densified, an
  arrow, "Bearing" info row) - each with `info` {key: value} for its popup.
  Input: the tool's own features ([lat, lon]), GeoJSON (Feature /
  FeatureCollection / every geometry type incl. Multi* and collections,
  [lon, lat]; simplestyle properties -> label / colour / width / fill, the
  rest -> info) or a file (.geojson / .json / .csv / .tsv - header with lat
  / latitude / y, lon / lng / long / longitude / x, a label column name /
  label / title / callsign / site / id, optional color; , ; or tab; quotes)
  from the conversation's folder or ai/knowledge only (realpath under them,
  <= 8 MB). Same name = replaced (`replace:false` adds); id = slug of the
  name. Limits: 30 layers, 5000 features / 20000 vertices a layer, 1 MB
  for all (one WebSocket message), text cut (label 80, info 24 rows x 300)
  - the tool's reply lists what was left out and why. Colours only #rrggbb
  (names mapped). Stored `ai/map_layers.json` (gitignored; a broken file ->
  `.bad`, re-checked feature by feature on load), sent as `{"ai_map":{seq,
  layers:[{id, name, desc, src, conv, col, t, f:[{g: pt|ln|pg|ci, c, r, l,
  col, ic, sz, w, da, fo, ar, x}]}]}}` after each change and on AI_STATE.
  `get_screen` lists them (`ai_map_layers` id / name / features). Page
  (kraken_doa.html "🤖 AI MAP LAYERS"; state `aimLayers` / `aimUi` in the
  globals block - mapDrawNow runs before the section): each feature
  projected once (`aimPrep`: P rings in normalized Mercator, kept
  continuous over the antimeridian; circles as 64 great-circle points; B
  box), `aimDraw` after the path loss, before the markers (areas, lines,
  points; labels while <= 400 points in view or zoom >= 13), click =
  popup (`mapSel` "ai:<layer>:<i>", `aimHitAt`: points 6 px + size, lines 5
  px, areas inside; info values through aiMd - escaped, links; position /
  where clicked, distance + bearing from the station, line length, circle
  radius; points: "📍 Add as marker" -> mkCreate). The map push's
  selection clean-up leaves "ai:" (it closed the popup within a second).
  Panel `#map-ai` under the viewshed's (`.aim-it` rows - the viewshed's
  `.vs-it` are hidden by its line of sight mode): show / hide per browser
  (localStorage `kraken_aim`), ⌖ fit, ✕ = `AI_MAP_DELETE:<id>` (confirm,
  every page). Map tool steps get 🗺 in the dock
- 🗺 Page commands (`page_call` in ai_mcp.cpp): the map's view and the
  Viewshed / path loss live in the browser, so these tools are carried
  out by A PAGE: pages announce a random id (`aiPageId`, `AI_PAGE:<id>` on
  connect) and send it with `AI_ASK` (`page`); AiManager::target_page() =
  the asking page while a question runs, else the last announced.
  `{"ai_page":{id (random), page, op, args}}` to every page, the one with
  that id answers `AI_PAGE_REPLY:{id, ack:true}` at once (8 s, else "the
  page didn't answer - closed / needs a reload") and `{id, ok, result |
  error}` when done (per op timeout; 300 s for coverage) - the worker
  thread waits on a condition variable, `ai_mcp::page_reply` on the uWS
  loop. Ops (`AI_PAGE_OPS` in the page): map_show (layer fit / box / point
  + zoom / station), vs_state, vs_set (mode, target height + reference,
  refraction, the station's height / radius, the signal settings by name
  -> plSet; reports applied / rejected), vs_add_tx (signal mode, a TX site,
  height / radius, centred), vs_remove (a site / all TX - never the
  station's), pl_link (a TX site -> a place; the strip opens), vs_query
  (points: ground, per worked-out site in sight + the height needed, in
  signal mode loss / received dBm / reliability - the hover's numbers).
  Each opens the map, starts the jobs and waits until nothing is pending
  (`aiVsBusy`: vsJob / plJob / a shown site without a current result /
  the link), then answers the state (`aiVsStateJson`: sites with % in sight
  / covered, ground, antenna ASL, cell; the radio settings; the link's
  budget - `aiLinkJson`). map_plot opens the asking page's map on the new
  layer (`page_tell`, fire and forget; show:false = not). An older
  receiver answers AI_PAGE with "unknown command" - the page ignores that
  error (an updated page on a not-yet-restarted receiver)
- WS: `AI_STATE` (-> `ai_state`: enabled, backend, model, model_sel,
  effort, defaults, cli, version,
  sandbox + why, busy, readonly, job, session {id,title,state}, log; +
  plugins, marks, sessions, codecs), `AI_ASK:{json}`, `AI_NEW`, `AI_TEST`,
  `AI_CANCEL`, `AI_READONLY`, `AI_MODEL`, `AI_EFFORT`, `AI_SESSIONS`,
  `AI_SESSION_GET / SELECT / DELETE:id`, `AI_MARKS` (-> `ai_marks`),
  `AI_MARK_DELETE:id`, `AI_PAGE:<id>`, `AI_PAGE_REPLY:{json}`, `AI_MAP_GET`
  (-> `ai_map`), `AI_MAP_DELETE:<id>`, `PLUGINS_*`; to pages also
  `ai_map`, `ai_page`; errors `{"ai_error":{cmd,error}}`. AI_INVESTIGATE /
  AI_CREATE are gone
- Gate unchanged: `ai/ai_config.json` `"enabled": true` only via `python3
  ai/kraken_ai.py setup` on the Pi; backend "command" (another CLI) gets the
  text only - no MCP, no tools, the last exchanges as context
- Windows (merge of 2026-10-20; `py -3 ai/kraken_ai.py setup` on the PC):
  the dock, MCP server, map_plot / map layers and conversations work
  (smoke-tested, below); capture_iq and the viewshed / path-loss tools are
  NOT yet tested on Windows (they need heimdall / a browser page); the
  sandboxes don't exist there. Claude Code has no sandbox on Windows ->
  `sandbox_status()` false ("Claude Code has no sandbox on Windows") -> no
  Bash for the agent (as on a Pi without bubblewrap). `aisandbox::available()`
  is always false (no bwrap): a plugin on the list is refused (`--info`,
  `PluginProcess::start`, test-signal generation while any is listed or a
  turn runs) with `aisandbox::NO_SANDBOX_FIX` ("Windows has none: check ...
  py -3 ai/kraken_ai.py trust <id>"); `changed_since` uses max(mtime,
  creation time) in s (no ctime; NTFS tunnels creation times), `real()`
  '/'-separates `_fullpath`'s result, the list's mtime comes from
  `fs_mtime_ms`, saves go through `fs_replace`. The bridge: permission rules
  in Claude Code's POSIX path form (`rule_path`: `//c/Users/...`), no
  /usr/include, paths in the prompt '/'-separated, "on a Windows PC" in the
  system prompt, `--context-file` when the command line would pass 32000
  characters (`cmdline_estimate`), `private_dir()` = icacls owner-only ACL on
  `ai/run`, `replace_file` (retries a target kraken_doa has open),
  SIGBREAK = Stop. kraken_doa: kproc for the bridge (env_unset of inherited
  `KRAKEN_MCP_*`, env_set URL / TOKEN + PYTHONUTF8), sigtool through
  `kproc::python_cmd`, the MCP token / mark IDs from `RAND_bytes`, the MCP
  listen socket `LIBUS_LISTEN_EXCLUSIVE_PORT` (Linux keeps uSockets'
  default), `read_input_file` takes drive paths, refuses shares
  (`fs_is_network_path`) and compares case-insensitively. Smoke test on
  Windows (2026-10-20): MCP 401 / 403 / 405, 33 tools, get_screen,
  remember / forget, map_plot over existing files, CSV from ai/knowledge by
  relative and drive path, `..` / share / outside path refused, a second
  bind of the port refused (WSAEACCES), a chat turn through a "command"
  backend (start / question / session / text / done / exit 0, the changed
  plugin folder put on the list), a 30 K context through --context-file, a
  listed plugin refused at the scan
- Windows, after the review of that merge (2026-10-20): `claude_path()`
  takes only a `claude.exe` (native installer); an npm `claude.cmd` shim
  runs through cmd.exe, whose 8191-character command line can't hold the
  ~10 K system prompt (and which re-parses & | % in it), so every question
  failed - `claude_missing()` names the shim in setup / test / the dock's
  error. The path rules hold on NTFS: a dummy turn with Claude Code 2.1.304
  had API_TOKEN / Api_Token / api_token. / "api_token " / API_TO~1 (8.3) /
  api_token::$DATA / plugins/../api_token reads and plugins/SDK/, Sdk/,
  sdk./, "sdk "/, foo/../sdk/, sdk::$INDEX_ALLOCATION/ writes all refused
  (the allowed write went through). Backstop anyway: `turn_finished` puts
  EVERY plugin on the list when `plugins/sdk`, `plugins/lib` or a file
  directly in `plugins/` changed during the turn (stderr line; a changed
  `ai/kraken_ai.py` / `ai/sigtool.py` is logged). kraken_doa deletes
  `ai/run/mcp-*.json` / `settings-*.json` when a job ends and at startup:
  a Stop without a console to share ends the job at once, the bridge's own
  cleanup never ran and the MCP token file stayed. Runtime test (command
  backend, scratch KRAKEN_PLUGIN_DIR, kraken_doa without a console and
  with a hidden one): 13/13 each - startup leftovers removed (other files
  kept), one changed plugin -> only it listed, sdk / top-level file ->
  all, no change -> none, Stop -> exit 143 (no console) / 1 (Ctrl+Break)
  and the leftovers removed
- Tests 2026-10-18 (scratchpad ai/): t_cap.py (capture_iq in kraken_doa,
  19/19, below), t_map.py (40/40: map_plot features / GeoJSON / CSV,
  refusals with reasons, files outside the folders refused, replace /
  append, the 5000 limit, map_layers / map_remove / AI_MAP_DELETE, kept over
  a restart, no page -> a clear error; with headless Chromium as the page:
  map_show, the panel, a click's popup escaped, hide / show; viewshed_get /
  _set line of sight from the station (~6 s with terrain from AWS),
  terrain_at, signal mode settings + rejected values, viewshed_add_tx
  coverage, path_loss to the station, terrain_at reliability,
  viewshed_remove, the station's refused; the ⚙ model / effort row; no page
  errors), t_map_e2e.py (a real question from the page, Sonnet at effort
  low picked in the ⚙ row: plotted 2 sites + a 330 deg bearing, checked
  line of sight with viewshed_set + terrain_at, answered with both - 27 s,
  $0.13; the usage event named claude-sonnet-5-5 / low)
- Tests (scratchpad ai/): t_mcp.py (protocol + security 46/46), t_perm.py
  (the permission layer with no prompt telling the model the rules: secrets
  never shown, no writes into src / sdk / lib / make files / home, writes
  allowed where they should be, + its shell steps - 16/16), t_bash_sbx2.py
  (30 shell commands in the REAL layout, the real doa_settings.json /
  server.key probed for readability only: secrets, ~/.ssh, ~/.claude, other
  processes, core-file / home / ai/ / ai/state writes, internet, 8092 / MCP
  ports all blocked, writes to a plugin / knowledge / its folder and a
  plugin build + --info allowed - 30/30), t_build_e2e.py (a real turn:
  "copy pocsag as zzcopy, build it" - Sonnet, 15 s, $0.07; marked, listed,
  its decoder under bwrap, trust - 11/11). A test receiver under /tmp is NOT
  a valid layout for the shell tests (writes into denyWrite paths under /tmp
  "succeeded" without changing the file), t_chat.py (real Claude: answers from the screen, add_vfo,
  Read only, refusals), t_ai_ui.py / t_ai_ui2.py (the dock in headless
  Chromium, 23/23), t_sbx.py (marking, bwrap'd decoder, trust), t_sonde.py
  ("look for an active weather balloon" on the radiosonde debug playback:
  Sonnet found both, decoded RS41 U2410853 + M20 512-2-14077 at 40 / 250
  deg, 62 s, $0.15 - the first run without peak hold found nothing). Run
  against a test build with its own ports (run_lq.sh: kraken 57080, MCP
  57079)
- IQ capture (`src/ai_capture.cpp`, `aicap::run`, 2026-10-17; it was
  `sigtool.py capture`: a second 8091 connection - heimdall sent its whole
  24 MB/s stream twice and sigtool kept one antenna - and numpy at 46-86 %
  of a Pi 5 core): the data receiver hands each packet's bytes of the wanted
  channel(s) to a queue (`aicap::on_packet`, right after the 📼 recorder's
  hook, before the retune skip - packets with the noise source on /
  retuning become gaps), the MCP tool's worker thread converts (LUT, DC
  tracked like the receiver's converter), mixes (phasor exact per packet)
  and decimates with the VFO decimator's Kaiser cascade on liquid's NEON
  `firdecim_crcf`, taps normalised to unity gain. Rate = 2.4 MHz / an
  integer, passband 0.8 x it. Files: `<base>.cf32` + `.json` (sigtool's
  sidecar: rf_hz, center_hz, offset_hz, rate, bandwidth_hz, gain_db, source,
  channel, calibration_gaps, skipped / dropped packets, stopped) - the CH
  selector's antenna or `antenna` (coherent), the tuner holding the
  frequency (independent / wideband scan); `all_antennas` (coherent, <= 400
  MB) also `<base>_array.cf32` = every antenna sample-major + a sidecar with
  `channels` and `array` {topology, radius_m, positions_m} - a plugin's
  `decoder --file <it> --uca R` reads it as is (per-talker bearings). One
  capture at a time; a retune (> 2 kHz in the header) / an element-count
  change / 5 s without data ends it early with what was captured; queue <=
  64 MB (then packets dropped = gaps). Cost (test build, Pi 5): one antenna
  +6 % of a core (12.5 kHz), every antenna at 48 kHz +30 %. Test: scratchpad
  ai/t_cap.py (19 checks: CW level = the old Python capture's to 0.01 dB,
  noise within 1 dB, bearings 60.0 / 200.0 from the array file, sigtool
  tones 1 kHz, gaps with calibration bursts every 2 s, a retune mid-way,
  refusals)
- `ai/sigtool.py` (numpy only; the assistant's `capture_iq` runs its
  `analyze`, its shell the rest): `capture` (standalone use; 8091 channel 0 - `--data-port` /
  `--control-port`, env `KRAKEN_HEIMDALL_DATA_PORT` / `_CONTROL_PORT` -,
  exact centre from 8092 get_status, packets with the noise source on / retuning are cut out,
  then mix + fast-convolution channelizer to 2.4 MHz / integer), `analyze`
  (bursts, in-passband spectrum stats, envelope, x^2/x^4 lines, FM
  discriminator histogram / transition lines / audio tones, PNG plots via a
  built-in rasterizer + 5x7 font), `spectrogram`, `extract`, `demod`,
  `tones`, `symbols` (per-segment best-phase sampling, k-means levels, eye
  ratio, repeated 24/32/48-bit words = sync candidates, frame period; every
  segment x 16 phases as one array - `kmeans_rows` - in batches of 256
  segments: 30 s at 4800 Bd 13.6 -> 1.9 s, output byte-identical; it was a
  Python loop of 11,500 tiny k-means runs)
- Memory (sigtool + `ai/memguard/`): the Pi has ~1 GB free with the stack
  running, and a Python process that outgrows it swaps the receiver to a
  standstill (two 45 / 50 s captures froze the Pi: capture held the whole
  recording plus float64 / complex128 copies, ~140 MB per second). So:
  - `capture` streams: a reader thread takes the 8091 packets off the
    socket, the main thread converts / DC-removes (smoothed piece means,
    ramped) / mixes (`mix(.., n0)`: blockwise, phase continuous) /
    channelizes (`Channelizer`: the overlap-save filter fed a piece at a
    time, same output as on the whole array) ~0.2 s at a time and appends
    to `<out>.part`, renamed when done (removed on error / SIGTERM). ~70 MB
    for any length (old: 481 MB for 3 s), keeps up with real time on a Pi
    5; `--seconds` <= 600, refused when the file wouldn't fit on the disk
  - analysis: cf32 inputs are `np.memmap`ped (page cache, not process
    memory); `extract` streams like capture; `demod` works in pieces (the
    demodulator, then a Channelizer to <= 2x the audio rate, a small
    resample at the end - one FFT over a 45 s recording alone took ~580 MB);
    `fm_disc`, levels and `bursts` run in blocks; `welch(.., fn=)` applies
    the envelope / x^k / mean removal per segment; `analyze`'s modulation
    statistics use at most `STATS_MAX` (2^22) samples of the signal (the
    report says so). 45 s at 480 kHz: analyze 1290 -> 378 MB, demod ~1 GB ->
    250 MB (both incl. 173 MB of the mapped file), symbols 9+ min -> 11 s (its
    `np.interp` built a recording-long axis per call). The SSB demod was
    also fixed (it took the real part 1.5 kHz off zero: a 1 kHz USB tone
    came out at 500 Hz)
  - ceiling: `memguard/kraken_memguard.py` `apply()` sets RLIMIT_DATA (heap
    + anonymous mappings = every numpy array) to MemAvailable minus
    max(400 MB, 10 % of RAM) (>= 256 MB; a tighter inherited limit is
    kept; `KRAKEN_AI_MEM_LIMIT=0` = none), so too much = MemoryError
    instead of a frozen Pi. sigtool applies it itself (MemoryError -> "work
    on a part / extract to a lower rate"); `kraken_ai.py` starts the agent
    with `memguard/` on PYTHONPATH (`agent_env()`), so its
    `sitecustomize.py` applies it to every Python process the agent runs
    (checked: the claude CLI passes PYTHONPATH to its Bash tool). The
    agent's system prompt says how to work on long recordings
- History: the Lab (its predecessor) was verified on live investigations
  (162.4 / 454.6 MHz) by the user, and investigate + create on a synthetic
  APRS recording produced the `aprs` plugin (5/5 frames, 0.5% CPU, quiet on
  noise) in ~5 min

