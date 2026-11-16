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
  `./run.sh stop` stops everything (`NO_AUTOBUILD=1`: don't build first,
  see *Automatic build*). Unknown arguments are an error (a
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
  `TMUX_SESSION` and stub `stop_stale`. A copy may also start a build and
  its progress page: set `NO_AUTOBUILD=1`, or move the page off the live
  ports with `BUILD_PAGE_HTTP_PORT` / `BUILD_PAGE_HTTPS_PORT` (see
  *Automatic build* below).
- `run.sh` from INSIDE its own tmux session (e.g. a window opened there):
  starting refuses (replacing the session would close that shell mid-start);
  `stop` stops the apps and sweeps stale processes, then closes the session
  (and that window) last. Only panes run.sh started are signalled
- heimdall saves the last USER tuning (frequency, gain) in
  `heimdall_settings.conf` once it has been stable for 10 s and restores it
  before the dongles open, so a crash restart comes back where the array was
  (scanner hops are not saved)
- Boot service (`install-pi-service.sh`): `Type=forking`, `ExecStop=run.sh
  stop`, `LimitRTPRIO=30` (realtime USB threads), `LimitMEMLOCK=infinity`
  (heimdall's `mlockall`), `Restart=on-failure`. install.sh grants the same
  two (`rtprio 30`, `memlock unlimited`) in limits.conf for login sessions.
- The web page's ↻ Restart / ⏻ Shut down (`kraken_doa_v2/docs/dev/power.md`,
  2026-11-16): `RESTART_EXIT=75` - both apps get `KRAKEN_RESTART_EXIT=75`
  from the supervisor (tmux mode, also under the boot service) and from
  headless mode; the supervisor restarts an app that exits with it after
  1 s (not RESTART_DELAY, no crash warning); headless mode, when heimdall
  exits with it, stops the client and runs `run_headless` again (logs
  appended, not truncated). `install-pi-service.sh` writes
  `/etc/sudoers.d/krakensdr-power` (checked with `visudo -cf` first, mode
  0440): `$RUN_USER ALL=(root) NOPASSWD: /usr/bin/systemctl poweroff,
  /usr/bin/systemctl reboot` (+ the `/bin/` paths) - the service has no
  login session, so logind refuses `systemctl poweroff` without it unless
  the user has sudo NOPASSWD ALL anyway (the Pi image default).
  `POWER_BUTTONS=0` removes the rule and puts `Environment=KRAKEN_POWER=restart`
  in the unit; `--uninstall` removes it.

## Automatic build (run.sh + scripts/autobuild.sh)

`./run.sh` builds heimdall and / or kraken_doa itself when they are not
built for the machine it runs on (it used to stop with "not built. Run
install.sh first"), with a temporary progress page on the web UI ports.
`NO_AUTOBUILD=1` restores the old behaviour (no check, no build). Added
2026-11-15.

Flow (run.sh):
1. `scripts/autobuild.sh check` on every start (read-only, ~10 ms on the
   x86_64 dev box, 18 small processes + ~1200 stats; not timed on a Pi). Its
   reasons are printed.
2. The old session is stopped and the stale-process sweep runs as before,
   then any leftover page server (`stop_build_page`: logs/build_page.pid,
   plus a `pgrep -f` for `build_page.py --status` whose pidfile isn't
   written - with the argument, so an editor with the file open is left
   alone). Only then is
   anything built: the ports are free and no running binary is relinked.
3. Build with the page up (`build_and_report`): a first status file is
   written (a stale "done" would make the page reload at once), the page
   server is started detached (`os.setsid` through a python one-liner -
   macOS has no `setsid` command) with `--http 8070 --https 8080 --cert/--key
   kraken_doa_v2/server.{crt,key} --pidfile logs/build_page.pid`, then
   `autobuild.sh build --status logs/build_status.json --log logs/build.log`
   runs in the background + `wait` (a TERM / Ctrl+C stops the build and the
   page). If the page server exits at once (ports busy, no ssl...) the build
   runs with terminal output only. After success: 2 s for the page to see
   "done", the server is stopped (it closes its ports within ~0.5 s), and the
   apps start as usual; the page reloads once its URL answers without its
   `X-KrakenSDR-Build` header (scripts/build_page.py) - on 8080 that is only
   after the convergence wait, as kraken_doa starts then.
4. tmux mode: the build runs inside the session, in a window `build`
   (`run.sh __build_pane`, settings passed by `env` - the pane gets the tmux
   server's environment, not run.sh's). When it is done it opens the stack's
   window (`launch_tmux_stack window`, the same panes as before) and exits;
   the window closes (remain-on-exit off for it). run.sh itself returns
   right after creating the session, as before. Headless (`NO_TMUX=1`): the
   build runs in the foreground, then `run_headless`.
5. `stop_session` and the stale sweep include `__build_pane` panes (their
   process group holds make and the compilers), and the sweep also stops the
   process group of any other `autobuild.sh build` of this tree (a headless
   run's build: its python runs as `python3 - <root>/scripts/autobuild.sh
   build`), so two starts never run two makes in one tree (tested: the first
   run ends "Build interrupted", the second builds alone); `run.sh stop` also
   stops the page server.

Failure policy:
- `autobuild.sh runnable` is evaluated per app BEFORE the build (the build
  rewrites the flags stamps before it relinks). After a failed build an app
  is usable if `check` now passes for it (it was built in this run) or it
  was runnable before and its executable is still there (a failed link
  deletes it: ld removes its output, make the target of an interrupted
  recipe - this used to announce "starting the OLDER build" with no heimdall
  left to start). Both usable -> a loud warning (stderr) with the error
  and the apps start (a pull that doesn't compile, no network).
- Otherwise the status stays "failed" with the error, the page server stays
  up showing it (detached, pidfile), the error is printed and run.sh exits 1
  (headless) / the build pane exits 1 and stays visible (remain-on-exit on
  is set for it then), so the session - and the boot service - stays up
  with the error on screen and on the page, without a restart loop. The next
  `./run.sh` or `run.sh stop` removes the page.
- "runnable" = the executable exists and no stamp names another CPU. A stamp
  from before the CPU part (no `| cpu`) is accepted here (most likely built
  on this machine; it may still crash with "Illegal instruction" if the
  tree was copied - then the supervisor's restart messages show it), while
  `check` treats it as "needs a build" (make rebuilds it too). A missing
  liquid-dsp build doesn't make kraken_doa unrunnable (linked statically).

Boot service: unchanged. With the build in a tmux window, run.sh's start
still returns in seconds, so `Type=forking` / `TimeoutStartSec=120` are not
hit by a 10-30 min first build on a Pi 4, and the kraken-term / tty1 viewers
find the session at once and show the build. A failed build leaves the
session (dead build pane) up: the unit stays active, `Restart=on-failure`
doesn't loop; the next boot or `systemctl restart krakensdr` tries again.
`ExecStop` (`run.sh stop`) also stops a build in progress and the page.
Headless mode (`NO_TMUX=1`) builds in the foreground and never forks, so it
doesn't fit that unit anyway.

### autobuild.sh check: detection rules

`make -q` is no use: the Makefiles have phony prerequisites (`check-uws`,
`install-uws`, `flags-changed`, `rtlsdr-check`...) that make it always
answer "out of date", and parsing them runs `$(shell)` side effects (the
uWebSockets pin switch with `git fetch`, liquid.mk's `rm -rf` of a build
for another CPU, librtlsdr git calls). `make -n` would even run `+` / `$(MAKE)`
recipe lines. So check compares stamps and file times itself (bash, no
make, no network, nothing written). An app needs a build when:
- its executable is missing;
- a flags stamp (`heimdall_v2/build/.flags`, `kraken_doa_v2/obj/.flags`,
  `kraken_doa_v2/plugins/lib/build/.flags`) is missing, or its `| cpu <id>`
  part isn't this machine's CPU (`HOST_CPU`: the same sed pipeline over
  /proc/cpuinfo as the Makefiles - keep them alike), or it names no CPU but
  says `native` (made before the CPU part existed), or it is newer than
  what is built from it (the executable; `libkrakendig.a` for the plugins'
  stamp): make rewrites a stamp at the START of a build for new flags or
  another CPU, so an interrupted build of a tree copied from another CPU
  otherwise left this CPU's stamp next to the other CPU's executable and
  looked up to date (tested: SIGINT 3 s into such a build);
- liquid-dsp: `libliquid.a` or its `build-kraken/commit` stamp missing, the
  commit isn't `LIQUID_COMMIT` of liquid.mk, or its ` cpu <id>` differs (or
  is missing on x86_64 / aarch64 / armv7l, where liquid.mk adds one);
- a source (`*.cpp *.hpp *.h *.c Makefile *.mk CXXFLAGS LICENSE`, `build`
  dirs pruned) is newer than the reference: heimdall `src external config.h
  Makefile`; kraken_doa `src include external Makefile liquid.mk plugins`
  (plus each app's `mac/` on macOS, force-included there);
  the reference is the newer of the executable and the success stamp
  (`build/.autobuild_ok` / `obj/.autobuild_ok`, touched to the build's START
  time after a successful build). The success stamp stops rebuild loops for
  a file check counts but make ignores (a plugin's `tools/*.cpp`, a plugin
  that only warns): one no-op make, then up to date (tested);
- uWebSockets missing, `uWebSockets/.git/HEAD` not the pinned `UWS_COMMIT`,
  or no `uSockets/*.o`;
- heimdall: `librtlsdr/build/src/librtlsdr.a` missing, a fork source newer
  than it, or `librtlsdr.c` without `rtlsdr_finish_tune` (RTLSDR_FIX_MARK).
Not covered (make doesn't rebuild for them either): compiler / system
library upgrades, whisper (optional, kraken_doa's make updates it). Also
not covered: a single plugin's missing `build/decoder` (e.g. a build
interrupted among the plugins; kraken_doa runs without it, make would build
it). A source dated in the future (a clock that went back, e.g. a Pi
without RTC booted offline after a pull) is built once, with a warning: the
success stamp is then dated past the newest such file, so later starts
don't rebuild until something newer arrives (before 2026-11-15's review fix
each start rebuilt until the clock passed it).

### Status file contract (read by scripts/build_page.py)

`logs/build_status.json`, rewritten atomically (`.tmp` + rename) every
0.5 s while building and on the end:

```
{"state": "building" | "done" | "failed", "percent": 0-100 (monotonic, < 100
 until "done"), "phase": "heimdall: librtlsdr" | "heimdall" |
 "kraken_doa: liquid-dsp" | "decoder plugins" | ..., "step": "Compiling
 src/dsp/correlation.cpp", "done": <steps finished>, "total": <steps
 planned>, "started": <epoch s>, "updated": <epoch s>, "error": "<one line,
 only when failed>", "log_tail": [<last 15 output lines, control chars
 replaced, <= 500 chars each>]}
```

JSON by Python's `json.dump(ensure_ascii=True)`. run.sh writes the first
"preparing" status itself before the server starts.

### Progress: how percent is computed

autobuild.sh's build mode is an embedded Python program that runs `make
-j$JOBS` per app (exactly install.sh's invocation and JOBS-by-RAM rule,
`JOBS` env override; `LC_MESSAGES=C` so the parsed messages are English,
LC_CTYPE untouched) and reads its output.
1. Plan: before make starts it lists the steps the build will run, each with
   a weight: objects from the Makefile's source list that are missing, older
   than their source or their `.d` dependencies (or all, when the flags
   stamp doesn't match), pffft objects, the links, the uWebSockets clone +
   uSockets, the librtlsdr clone + cmake build, liquid-dsp, every
   plugins/lib object, the plugin host, the codec check and every plugin
   decoder that is missing or older than its files / the shared headers.
2. Weights, in units of one kraken_doa object, measured in a fresh build
   on the x86_64 dev box at -j3 (one kraken_doa object = 3.2 s of one job
   slot): heimdall object 0.55, pffft 0.2, plugins/lib object 0.6, a plugin
   0.3 + 0.021 per KB of its .cpp/.hpp (its single g++ run measured 1 s +
   0.065 s/KB across the 28 plugins, ism 310 KB = 22 s, v23_telemetry 18 KB
   = 1.2 s), liquid-dsp 7 (fetch + configure + 159 C files), a git clone
   1.5 (network bound, so worth relatively less on a slow CPU), uSockets
   0.5, librtlsdr 2, a link 0.5. A fresh build plans ~134 steps.
3. Live: "Compiling X..." / the plugin g++ lines (`-o <id>/build/decoder.tmp`,
   `-o lib/build/x.o`) start a step. Make prints at the start of a job, so
   finishing is inferred: with all JOBS slots busy, a new start means the
   oldest running step ended (FIFO); explicit end markers override that
   (`Linking heimdall/kraken_doa` = all its objects, `ar rcs ...libkrakendig.a`
   = the lib objects, `built plugin X`, `[100%] Built target rtlsdr_static`,
   uSockets' `Leaving directory`, liquid's `(N objects)` line, the end of
   each make = every step of that app). librtlsdr reports cmake's `[ NN%]`
   on make's output; liquid-dsp's cmake writes to
   `liquid-dsp/build-kraken.log`, which is polled for its `[ NN%]`.
   A running step without a percentage earns time-based credit (elapsed /
   expected, capped at 85 %; expected = weight x the seconds per unit learned
   from the finished steps, 10 s before the first one ends).
4. percent = done weight / planned weight, never decreasing, <= 99 until
   make has finished; a step that wasn't planned is added when it starts.

Measured (x86_64, i7-11800H, JOBS=3, fresh tree incl. clones, 147 s):
percent tracked the elapsed share within ~2 points the whole way (12 % at
22 s, 32 % at 51 s, 51 % at 74 s, 65 % at 97 s, 81 % at 121 s, 95 % at
139 s); the longest stretch without a percent change was 4 s. Not measured
on a Pi: there the clones are relatively shorter and the compiles longer,
which the weights only approximate.

Error text (`error`, one line): missing build tools are checked first
(make, g++, gcc, cmake, git, pkg-config); after a failed make the output is
searched for a missing header / `-l` library / tool / pkg-config package /
CMake package (-> "... is missing ... - run ./install.sh once"), then a
failed download ("network needed for the first build"), then the first
compiler `error:` line. autobuild never runs sudo, apt or brew.

### Tested (2026-11-15, x86_64 dev box, worktree, stubbed run.sh copy)

With a copy of run.sh (`TMUX_SESSION=autobuild_test`, a guard refusing
any other session, `stop_stale` and the app launches stubbed, page on
18070 / 18443): check / runnable for every rule above (another CPU in each
of the four stamps, a stamp without CPU, missing executable, touched
sources, missing uSockets objects, touched librtlsdr.c); a fresh full build
(trace above); headless build with the page polled (HTTP and HTTPS answer,
status moves to done, the server is gone afterwards); tmux mode (build
window -> stack window, the build window closes, `stop`); a compile error
with the older build runnable (warning, apps started) and with no
executable (exit 1, page keeps showing the error, `stop` removes it); the
same in tmux mode (dead build pane kept, page up); SIGTERM mid-build (make
and compilers gone, page stopped, status "interrupted"); both page ports
busy (terminal-only build); cmake missing from PATH, a missing header, an
unreachable uWebSockets URL (`UWS_REPO=` override), a no-op build.
Not tested: a real Raspberry Pi (timings, the check's speed there, a boot
with the service), macOS and its bash 3.2 (the scripts avoid bash 4
features, but they were only run with bash 5.1), starting the real apps
after the build (the launch was stubbed; the hand-off is `run_headless`,
unchanged, and `launch_tmux_stack`, the old inline tmux commands moved into
a function plus a `window` mode), the page reloading into the real UI in a
browser.

## Docker (DOCKER.md)

- `Dockerfile` (one image: both apps, librtlsdr fork + uWebSockets cloned at
  pinned commits, liquid-dsp built by `kraken_doa_v2/liquid.mk` in a layer of
  its own, runtime packages derived from `ldd`) + `docker-compose.yml`
  (two containers, `heimdall` and `kraken_doa`, `network_mode: host`,
  `restart: unless-stopped` = autostart at boot) + `docker/entrypoint.sh`
- USB passthrough: `/dev/bus/usb` bind-mounted + `device_cgroup_rules: c
  189:* rmw` (follows re-enumeration after heimdall's USB reset / a replug,
  unlike `devices:`); `ulimits: rtprio: 30` for the SCHED_RR threads,
  `memlock: -1` for heimdall's `mlockall`
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
- `KRAKEN_RESTART_EXIT: "75"` in both services: the page's Restart software
  ends both apps with 75 and `restart: unless-stopped` starts them again
  (tini passes the status on). kraken_doa sees `/.dockerenv` and offers no
  reboot / shut down (a container can't power the host off)


## Logging

Both apps write informational messages to stdout and errors/abnormal events to
stderr. On an interactive terminal, stdout drives the live StatusDashboard TUI
(or the plain scrolling logs with `HEIMDALL_NO_TUI` / `KRAKEN_DOA_NO_TUI`).
When stdout is NOT a terminal (redirected to a logfile or journal, e.g.
`run.sh` headless mode), stdout is **discarded at startup** so a deployment
that runs for years accumulates an errors-only log that cannot grow unbounded
from routine output. Set `HEIMDALL_VERBOSE_LOG=1` / `KRAKEN_DOA_VERBOSE_LOG=1`
to restore full output to files for debugging.

Each dashboard's SYSTEM line shows two CPU figures on one scale, the share of
the whole machine (every core busy = 100 %, one full core of a Pi's four =
25 %): `cpu heimdall` / `cpu kraken_doa` is what that program uses, `total`
is the machine's load from everything. kraken_doa's figure includes its
child processes (decoder plugins, the whisper / AI / test-signal helpers and
what those start), heimdall has none. `top` shows a process in percent of ONE
core: divide by the core count to compare. The counters are
`platform::process_cpu_ns`, `process_tree_cpu_ns` and `cpu_count`
(`platform.hpp`, shared by both apps); the process-tree one reads the process
list, so kraken_doa samples it once a second and only for the dashboard (the
web page's CPU figure is still the machine total). Measured on x86_64 Linux
(2026-11-15, 5-channel KrakenSDR): one process-list sample takes 1.4 ms with
~390 processes; both figures agree with `top` / `ps` (heimdall 50.8 % of a
core = 3.2 % of 16 cores; kraken_doa 1.7 %, 2.2 % with an RDS decoder
plugin running, 1.6 % after it stopped). The Windows (Toolhelp snapshot,
`GetProcessTimes`) and macOS (`sysctl KERN_PROC`, `proc_pid_rusage`)
versions are compile-checked only, not yet run.

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
- **Map provider API keys** (the web UI's ⚙ Settings: Google, Mapbox,
  Azure, MapTiler, ...) live only in the browser's localStorage, never in a
  file of the tree. The pre-push hook still blocks an outgoing commit that
  ADDS a recognisable one to any file (`MAP_KEY_AWK`): a Google `AIza…`
  key or a Mapbox `pk.` / `sk.` / `tk.eyJ…` token anywhere; a key in a URL
  query (`access_token=` / `apikey=` / `api_key=` / `subscription-key=` /
  `key=` with 20+ key characters); a provider entry of a pasted
  `kraken_map_prov` value, one-line or pretty-printed (`"azure": "…"`,
  likewise `mapbox` / `google` / `maptiler` / `thunderforest` / `stadia` /
  `here` / `custom`, 16+ key characters). A value counts only if it looks
  random - a digit and a letter, not `YOUR…` / `EXAMPLE` / `XXXX` / an
  ALL_CAPS or snake_case name - so placeholders and `${c.k}` / `{key}`
  templates pass. NOT recognisable: a bare Azure / HERE / MapTiler /
  Thunderforest / Stadia key in any other context (plain random strings).
  The hook prints file:line and the first 6 characters only. A legitimate
  line (a test vector) passes with the marker `map-key-ok` on the same line,
  instead of `--no-verify`, which would skip every other check too. Only
  lines new in the outgoing commits count, and a merge only for lines new
  relative to all its parents (combined diff), so a key already on the
  remote being pushed to blocks neither the push that deletes it nor a
  merge of the branch that has it (a commit that is only on some other
  remote is still scanned). A key that was ever pushed must be revoked at the provider, since
  removing it from history doesn't make it secret again.
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

