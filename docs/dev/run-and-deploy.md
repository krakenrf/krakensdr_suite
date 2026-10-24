# Running and deploying the stack

Moved verbatim from the root `CLAUDE.md` on 2026-10-24 (that file now keeps only
what every session needs). Not loaded automatically - read it when working in
this area. Where this and the component `CLAUDE.md` files disagree, the code and
the component files win.

Contents: run.sh / install.sh, Docker, logging, network access checks,
git secret guards.

## Running the Stack (install.sh / run.sh)

- `./install.sh` installs apt dependencies, builds the librtlsdr fork (see
  *Dependencies*) and both apps. It does NOT remove distro RTL-SDR packages -
  heimdall links the fork statically, so gqrx, gr-osmosdr, rtl_433 etc. can
  stay installed on the stock library.
- `./run.sh [--wideband|-w] [--kerberos] [--kerberos_sw|--kerberos-sw] [--ext_noise|--ext-noise]` starts
  both apps in a tmux split (`NO_TMUX=1` = headless, logs in `logs/`);
  `./run.sh stop` stops everything. Unknown arguments are an error (a
  mistyped flag used to be ignored silently); `-h` prints usage. heimdall
  and kraken_doa themselves also exit 1 on an unknown or incomplete option
  (heimdall accepts `--kerberos-sw` as well as `--kerberos_sw`).
- A heimdall startup failure after the dongles open (busy 8091/8092, device
  init, downconverter) releases the hardware and `_Exit(1)`s like the normal
  shutdown. A busy 1234 is NOT fatal: heimdall runs without the optional
  RTL-TCP tap (a stock rtl_tcp on a spare dongle uses that port) - a plain `return` ran global destructors that joined the
  RTL-TCP thread blocked in `accept()` and hung. A web port that can't be
  bound also ends with status 1. The shutdown waits (<= 5 s) for an
  element-count change holding `settings_mutex`, so it can't close handles a
  just-started reader is using.
- Each app runs under a supervisor (`run.sh __supervise`) that restarts it
  after a crash. Ctrl+C, `run.sh stop`, `systemctl stop` and closing the tmux
  pane/window all stop it cleanly: the supervisor runs the app as a
  background child it `wait`s on and forwards the signal, because a pane close
  SIGHUPs only the supervisor (the pane's session leader), never the app.
- Starting AND `stop` sweep stale processes first: any heimdall / kraken_doa /
  run.sh supervisor outside the current session (a dead tmux server, an old
  run, a headless run) gets SIGTERM per process group (clean shutdown,
  dongles released), SIGKILL after 10 s. Processes of other users are only
  reported.
- The convergence wait before the client starts ends at once when heimdall
  runs in wideband scan / independent mode (8091 phase-state bits 0x400 /
  0x800, probe exit 3): no calibration runs there - it used to hold the client
  back for the whole timeout, and headless mode then gave up on the stack
- The convergence wait before the client starts watches heimdall's process:
  headless mode aborts with heimdall's log tail if it dies; in tmux mode the
  client pane gives up if the heimdall pane's supervisor exits (a crash is
  restarted by the supervisor, so the wait continues through it).
- Headless mode has no supervisor: when heimdall ends on its own, run.sh
  exits with heimdall's status (1 = startup failure, 128+N = killed by signal
  N, e.g. 139 for a crash); a stop it was asked for (Ctrl+C / TERM) exits 0.
- Testing a COPY of run.sh still stops the live stack: before starting it
  replaces any tmux session named `$TMUX_SESSION` (default `krakensdr`) and
  sweeps stale heimdall/kraken_doa processes - give the copy its own
  `TMUX_SESSION` and stub `stop_stale`.
- `run.sh` from INSIDE its own tmux session (e.g. a window opened there):
  starting refuses (replacing the session would close that shell mid-start);
  `stop` stops the apps and sweeps stale processes, then closes the session
  (and that window) last. Only panes run.sh started are signalled
- heimdall saves the last USER tuning (frequency, gain) in
  `heimdall_settings.conf` once it has been stable for 10 s and restores it
  before the dongles open, so a crash restart comes back where the array was
  (scanner hops are not saved)
- Boot service (`install-pi-service.sh`): `Type=forking`, `ExecStop=run.sh
  stop`, `LimitRTPRIO=30` (realtime USB threads), `Restart=on-failure`.

## Docker (DOCKER.md)

- `Dockerfile` (one image: both apps, librtlsdr fork + uWebSockets cloned at
  pinned commits, liquid-dsp built by `kraken_doa_v2/liquid.mk` in a layer of
  its own, runtime packages derived from `ldd`) + `docker-compose.yml`
  (two containers, `heimdall` and `kraken_doa`, `network_mode: host`,
  `restart: unless-stopped` = autostart at boot) + `docker/entrypoint.sh`
- USB passthrough: `/dev/bus/usb` bind-mounted + `device_cgroup_rules: c
  189:* rmw` (follows re-enumeration after heimdall's USB reset / a replug,
  unlike `devices:`); `ulimits: rtprio: 30` for the SCHED_RR threads
- Each app runs with cwd `/data/<app>` (= `docker-data/<app>` on the host),
  web files symlinked in - every runtime file path in both apps is
  cwd-relative, so a new persisted file lands in the volume automatically; a
  new STATIC file the apps serve must be added to the Dockerfile COPY, the
  entrypoint's `link_files` and the `.dockerignore` allow-list
- `.dockerignore` is an allow-list: secrets (`doa_settings.json`, `api_token`,
  `server.key`) can't reach the image. `docker-data/` and `.env` are gitignored
- heimdall's entrypoint waits for `KRAKEN_TUNERS` dongles (boot enumeration);
  kraken_doa's waits for CONVERGED from the 8091 header (not in KerberosSDR
  manual mode, nor in wideband / independent mode - header bits 0x400 /
  0x800 - where no calibration runs); empty compose variables are unset (apps treat "" as set)
- `--wideband` / `--kerberos_sw` need `privileged: true` (hidraw, gpiochip,
  `/proc/device-tree` is masked otherwise) via `compose.override.yaml`


## Logging

Both apps write informational messages to stdout and errors/abnormal events to
stderr. On an interactive terminal, stdout drives the live StatusDashboard TUI
(or the plain scrolling logs with `HEIMDALL_NO_TUI` / `KRAKEN_DOA_NO_TUI`).
When stdout is NOT a terminal (redirected to a logfile or journal, e.g.
`run.sh` headless mode), stdout is **discarded at startup** so a deployment
that runs for years accumulates an errors-only log that cannot grow unbounded
from routine output. Set `HEIMDALL_VERBOSE_LOG=1` / `KRAKEN_DOA_VERBOSE_LOG=1`
to restore full output to files for debugging.

All cout/cerr output is made terminal-safe (`SanitizingBuf` installed in
each `main()`, and the TUI's log capture): log lines carry client-chosen
strings (WebSocket commands and close reasons, filenames, settings values),
so C0/C1 control characters (ESC, `\r`, BEL, ...), DEL and invalid UTF-8 are
replaced with `?` - an escape sequence can't recolor/rewrite the terminal,
TUI or a `tail`ed log file. Valid UTF-8 passes through. The apps' own log
lines must therefore not contain escape codes (the dashboards write theirs
straight to the terminal fd). Header: `heimdall_v2/src/core/log_sanitize.hpp`
= `kraken_doa_v2/include/utils/log_sanitize.hpp` (keep them identical).

Everything that indicates a fault goes to stderr and therefore still reaches
the log: coherence loss + recovery progress, periodic-calibration DRIFT,
locked-channel lag drift, device/GPIO/downconverter failures, client
connection loss, fatal startup errors. Recurring per-broadcast warnings in the
TCP data server are throttled (5 s) so a persistent fault cannot flood the
log. `run.sh` headless mode truncates `logs/*.log` at each start; a few
static-constructor lines printed before `main()` still land there (bounded,
once per start).

## Network Access Checks

- **Host allow-list (DNS rebinding):** heimdall's WebSocket/POST routes and the
  client's WebSocket upgrade, `/recordings` and 8081 page only answer requests
  addressed by an IP literal, `localhost`, this machine's hostname (bare or
  `.local`/`.lan`/`.home`/`.localdomain`/`.home.arpa`), or a name listed in
  `KRAKEN_ALLOWED_HOSTS` (comma-separated, both apps - set it when reaching
  the Pi through a reverse proxy or a custom DNS name). Header:
  `heimdall_v2/src/core/host_check.hpp` = `kraken_doa_v2/include/utils/host_check.hpp`
- **Same-origin:** both apps refuse a browser request whose `Origin` doesn't
  match its `Host` (cross-site WebSocket hijacking); requests without `Origin`
  (native apps, curl) only need the host check
- **API token (client, optional):** unauthenticated sockets get 10 s to send
  `AUTH:`, at most 8 may wait at once, and an IP with 5 wrong tokens in 60 s
  is refused for the rest of that window
- **Control port 8092** has no auth (LAN tools / the DoA client) but hangs up
  on any HTTP request: a web page's fetch/form POST to it would otherwise have
  its headers skipped and a JSON body run as a command
- **Local web-mapper port 8021** (opt-in) checks the Host allow-list but no
  Origin: it exists for mapper pages served from elsewhere, so anything on the
  LAN can read it by design
- **8081 `DOA_value.html`** stays token-free (the Android app can't send one)
  but has no CORS header, so other web pages can't read it

## Git Secret Guards

Station secrets (KrakenPro API key, latitude/longitude) are kept out of git
automatically:

- **Clean filter** (`.gitattributes` → `scripts/git-secrets-clean`): when a
  settings/config JSON is committed, the blob git stores has
  `krakenpro_key`/`web_mapper_key` emptied and `latitude`/`longitude`/
  `static_location` zeroed. The WORKING file keeps its real values.
- **Pre-push hook** (`.githooks/pre-push`): backstop that scans outgoing
  commits and blocks the push if a key / real coordinates / a tracked
  `doa_settings.json` or `api_token` slipped through (bypass:
  `git push --no-verify`). Root commits and merge commits (including a
  secret added while resolving a merge) are scanned too.
- **Voice codecs / binaries** never go to the remote (licensing: the ETSI
  TETRA codec may not be redistributed, AMBE is DVSI's): `.gitignore`
  ignores the user-built codec programs / libraries (`cdecoder`,
  `tetra-*decoder[.exe]`, `libmbe*`), the ETSI zip and osmo-tetra / mbelib
  checkouts in any folder; the pre-push hook blocks outgoing commits that
  add one anyway (`git add -f`), any `*.exe` / `*.dll` (everything is built
  from source) or the ETSI sources. `windows/package.sh` copies a fixed
  list of files, so a codec next to kraken_doa.exe doesn't reach the
  package either
- Filters and hooks are per-clone git config, so `install.sh` registers them
  (`filter.kraken-secrets.*`, `core.hooksPath .githooks`). A clone that never
  ran install.sh silently skips both — run it once after cloning.
- `doa_settings.json` (runtime home of the key + station location) is
  gitignored and must stay untracked.

