# ↻ Restart / ⏻ Shut down from the web page

Added 2026-11-16 at the user's request: "a safe shutdown and restart button
from the browser". Files: `src/power.cpp`, `include/power.hpp`, the
dispatcher hook in `src/control_handler.cpp`, the page's "⏻ POWER" section,
heimdall's `restart` control command (`heimdall_v2/src/net/tcp_control_server.cpp`),
run.sh's supervisor / headless loop, `windows/krakensdr.cpp`,
`docker-compose.yml`, `install-pi-service.sh`.

## What it does

- **↻ Restart** opens a dialog with *Restart software* (heimdall and
  kraken_doa start again; the array recalibrates, ~30 s) and *Reboot
  computer* - each only where it works. **⏻ Shut down** asks for a
  confirmation and powers the computer off, so a Pi can be unplugged once its
  green LED has stopped blinking.
- "Safe": before acting, kraken_doa stops a running IQ / audio recording
  (`RECORDER_STOP`, through the dispatcher, so every page sees it) and waits
  until its files are complete (`rec::writing()`, <= 20 s: the rest of the
  queue, the SigMF metadata), stops the DoA recording (`LOG_STOP`), then
  writes the settings (`SettingsStore::flush`), the decoder log's buffer
  (`declog::flush`, new), incidents and noise floors. Voice to Text appends
  each transcript as it is made (`transcripts.jsonl`), so it has nothing
  buffered. Then the action. The OS's own SIGTERM still runs kraken_doa's
  full shutdown afterwards (everything above is idempotent).
- Every open page gets `{"power":{...}}` with `busy` / `phase` and shows a
  notice over the whole page: "Shutting down… / Safe to unplug when the
  green LED stops blinking", "Restarting… / This page reconnects by itself",
  "Rebooting…". During the preparation the second line is "Finishing
  recordings, saving". The page's WebSocket reconnects every 2 s as always;
  when it is open again the notice goes (`pwOnOpen`, also with an older
  receiver that sends no power state). A failure shows "Not shut down" /
  "Not restarted" / "Not rebooted" with the reason and an OK button - only on
  the page that asked (`for` = the action) and on pages already showing the
  notice.

## Protocol

- Page -> receiver: `POWER:restart` | `POWER:reboot` | `POWER:shutdown`.
- Receiver -> pages: `{"power":{"actions":["restart","reboot","shutdown"],
  "busy":"" | <action>, "phase":"" | "preparing" | "running",
  "dry_run":bool[, "error":..., "for":<action>][, "note":...]}}` - in the hello
  every connecting page gets (`get_connect_sync_messages`), when the startup
  check finishes, and on every change. `actions` lists only what this
  installation offers; the page shows the Restart button for `restart` or
  `reboot`, Shut down for `shutdown`.
- Compatibility: an older receiver sends no `power` -> no buttons. An older
  page ignores the message (unknown key) and never sends `POWER:`.

## Who may send it

- Only a page's own WebSocket: `websocket_server.cpp` calls
  `ControlHandler::handle_page_message`, which sets a thread_local flag that
  `apply_command` takes (`std::exchange`) before anything else, so a command
  dispatched from inside another one (RESET_SETTINGS' defaults, the sequence's
  own RECORDER_STOP) doesn't inherit it. Every other caller of
  `apply_command` / `handle_websocket_message` - the settings replay, the AI
  assistant's MCP tools, the web mapper's cloud settings, the scanners, a
  debug-playback test transmission's `commands` (from a plugin's
  `testsignals.json`, run through `run_internal`) - is refused ("only the
  page's own buttons may do this").
- The AI assistant also refuses it in `ai_mcp.cpp`'s `cmd()` with a message
  for the model; none of its tools builds a `POWER:` command anyway.
- Not persisted: `POWER:` is not in the settings schema, is handled before
  the generic echo / save path, and the replay is refused even if a file
  contained it. Not echoed as `sync_cmd`.
- API token: unauthenticated sockets' messages are dropped before the
  dispatcher, so with a token set only authenticated pages can do it.
  Without a token anyone who can open the page can - as with every other
  control (tuning, deleting recordings); `KRAKEN_NO_POWER=1` for
  installations that don't want it on the network.
- Repeats: one action at a time (a second is refused "... already in
  progress"); a new attempt within 3 s of the end of the last one is refused
  ("too soon"); unknown actions are refused.
- Debug playback / playback of a recording: allowed. Restart and shut down
  are not settings (the sandbox is untouched: the user's own settings file
  is what the next start loads) and the heimdall `restart` goes over a
  connection of its own, not `send_control_command` (which the playback
  blocks). A recording can't run during a playback anyway.

## Per platform

| | Restart software | Reboot / Shut down |
|---|---|---|
| Linux, `run.sh` (tmux, also under the boot service) | yes: supervisors restart on exit 75 | `systemctl reboot\|poweroff` if logind says `yes` (an active local session), else `sudo -n systemctl ...` if `sudo -n -l` allows it |
| Linux, `NO_TMUX=1 ./run.sh` | yes: the headless loop restarts the stack when heimdall exits 75 | as above |
| Pi boot service (`install-pi-service.sh`) | yes (tmux supervisors) | `sudo -n systemctl ...` through `/etc/sudoers.d/krakensdr-power` (or the Pi user's NOPASSWD ALL) |
| Docker (`docker-compose.yml`) | yes: exit 75 + `restart: unless-stopped` | never (`/.dockerenv`, `/run/.containerenv` or `$container`) |
| Windows, `krakensdr.exe` (windowed) | yes: its supervisors restart on 75 | `%SystemRoot%\System32\shutdown.exe /r\|/s /t 0` when the token has SeShutdownPrivilege |
| Windows, `krakensdr.exe --headless` | no (not set) | as above |
| macOS, `run.sh` | yes | only with a sudo rule for `/sbin/shutdown -r\|-h now` (nothing installs one) |
| kraken_doa started by hand | no (`KRAKEN_RESTART_EXIT` unset) | as for the platform |
| non-systemd Linux | as for the launcher | no |

### Restart software: exit status 75

- No privilege is needed: every launcher already restarts a crashed app.
  The launcher passes `KRAKEN_RESTART_EXIT=75` (2-125; 1 is a startup
  failure, 126+ the shell's) to both apps, and treats that status as "start
  it again after 1 s" (no crash warning, not RESTART_DELAY).
- kraken_doa offers `restart` only when it has the variable. It first sends
  `{"command":"restart"}` to heimdall on a TCP connection of its own
  (127.0.0.1:`TCP_CONTROL_PORT`; heimdall answers every control client, and
  its 2 Hz status lines, which carry no `status` key, are skipped) and waits
  <= 4 s for the reply. heimdall accepts only when its own environment has
  the variable (else nothing would start it again), answers
  `{"status":"success","message":"restarting"}`, clears `global_running` and
  exits with that status after its normal shutdown (dongles released).
  Only then does kraken_doa set its exit status, clear `running` and run its
  normal shutdown (`main`: `platform::fast_exit(power::exit_code())`).
  heimdall refusing / not running / too old ("Unknown command") -> nothing
  restarts, the page shows why. So both restart or neither.
- run.sh tmux mode: each pane's supervisor restarts its app. The client
  doesn't wait for convergence again; it reconnects to heimdall as after any
  heimdall restart. Headless: heimdall's 75 ends `run_headless` (the client
  is stopped, it is exiting anyway), the loop starts the whole stack again,
  the logs are appended instead of truncated.
- The boot service needs nothing extra: the apps run in its tmux session, the
  tmux server (the unit's main process) stays.
- Docker: tini (`init: true`) passes the status on; the restart policy
  restarts the container, kraken_doa's entrypoint waits for convergence
  again.

### Reboot / shut down: what may this user do?

`probe()` runs once at startup on a worker thread (pages get the buttons
when it is done) and logs one line, e.g. `Power buttons: restart (exit
status 75), reboot (sudo), shutdown (sudo)` or `... - not offered: reboot
(this user may not (install-pi-service.sh adds a sudo rule))`. Linux order:
container -> no; no `/run/systemd/system` -> no; `busctl call
org.freedesktop.login1 ... CanReboot / CanPowerOff` = `"yes"` -> plain
`systemctl` (polkit allows an active local session: a desktop user running
`run.sh` in a terminal); else `sudo -n -l <systemctl> poweroff|reboot` exit 0
-> `sudo -n <systemctl> ...`. logind's default polkit policy needs an
active local session; for the boot service (no session) it answers
`challenge` (expected - not seen on a Pi yet; the dev box's test process,
outside a session, got `challenge`): that is why the Pi needs the sudo rule.

`install-pi-service.sh` writes `/etc/sudoers.d/krakensdr-power` (validated
with `visudo -cf` before it is installed, mode 0440, removed by
`--uninstall`):

```
<user> ALL=(root) NOPASSWD: /usr/bin/systemctl poweroff, /usr/bin/systemctl reboot, /bin/systemctl poweroff, /bin/systemctl reboot
```

Exactly those commands, nothing else. Why sudo and not a polkit rule:
Raspberry Pi OS Bullseye ships polkit 0.105 (no JavaScript rules, and its
`.pkla` files can't limit `manage-units` to one unit); a sudoers line works
on every Debian / Ubuntu / Pi OS, its effect is equally narrow, and `sudo -n
-l` is an exact runtime check. On a stock Pi the first user has `NOPASSWD:
ALL` anyway, so the buttons work there even without the rule;
`POWER_BUTTONS=0 ./install-pi-service.sh` removes the rule AND sets
`Environment=KRAKEN_POWER=restart` in the unit, which hides them. install.sh
changes nothing (a desktop user's session already lets logind decide).
`systemctl poweroff` can be refused by a "block" inhibitor or other
logged-in users (not tried) - then it exits non-zero and the page shows its
output.

The executor (`run_power_command`, the one place a power command runs)
starts it through `kproc` (stdin null, stdout + stderr captured), waits
<= 25 s for it to exit, and treats exit 0 as "taken". The pages then show
the running phase; the OS's SIGTERM ends kraken_doa normally. If this
process still runs 120 s later (an inhibitor delay, a hung shutdown), the
pages get "Still running 2 minutes after the shutdown command" and the
buttons work again.

## Environment

| Variable | Effect |
|---|---|
| `KRAKEN_NO_POWER=1` | no buttons, every `POWER:` refused, no probe |
| `KRAKEN_POWER=restart,reboot,shutdown` | the actions offered at most (`KRAKEN_POWER=restart`: no reboot / shutdown, no probe) |
| `KRAKEN_POWER_DRY_RUN=1` | everything allowed by `KRAKEN_POWER` is offered; the whole sequence runs (recordings finished, files written, pages told), but the command is only logged: `Power: dry run - would run: /usr/bin/sudo -n /usr/bin/systemctl poweroff`, or when this user may not: `... this user may not run it, the command would be: systemctl poweroff`; restart: `... would send {"command":"restart"} to heimdall (127.0.0.1:8092), then exit with status 75`. The pages show a "Dry run" badge and, 5 s later, "Dry run - nothing was done" |
| `KRAKEN_RESTART_EXIT=<2..125>` | set by the launchers (both apps): the status that restarts the app |

Log lines are information (stdout; deployed logs keep stderr only):
requested / refused / dry run / the command run. Faults go to stderr: a
failed command (`Power: shutdown failed: ...`), a recording still finishing
after 20 s, "still running 120 s after".

## Tests (2026-11-16, x86_64 desktop, TEST build on ports 2818x / 2819x)

The executor and the sudo probe were patched in the test copy to only log
(checked before every start: the source bodies and the strings in the
binary); `KRAKEN_TEST_PRETEND_SUDO` in that patch made the probe report a
sudo rule without running sudo. Nothing ran `sudo`, `systemctl` or
`shutdown`; the only probe that ran was the read-only `busctl ... CanReboot
/ CanPowerOff` (answer here: `challenge`).

- Headless Chromium, real mouse / key events, 111 checks: buttons only with
  the capability (dry run: all three; `KRAKEN_NO_POWER`: none and the command
  refused; `KRAKEN_POWER=restart` without a supervisor: none; with
  `KRAKEN_RESTART_EXIT`: Restart only, the dialog only "Restart software";
  pretend sudo + `KRAKEN_POWER=shutdown,reboot`: the dialog only "Reboot
  computer"); the row's place (above the layout row, footer box); both
  dialogs (screenshots looked at), cancel by the button, Esc, the backdrop
  and the X send nothing; shutdown / restart / reboot in dry run: notice on
  the asking page and on a second page, the dry-run lines in the log, the
  note and OK; "too soon" shown only on the asking page; a repeat while busy
  refused; an unknown action refused; nothing in `doa_settings.json`; the
  real executor path logs `/usr/bin/sudo -n /usr/bin/systemctl poweroff`,
  SIGTERM during "Shutting down…" exits 0, the notice stays while the
  server is down and goes when it is back; restart: heimdall not running
  ("heimdall is not running ..."), a fake heimdall refusing (message
  shown, kraken_doa keeps running), a fake heimdall accepting -> kraken_doa
  exits with status 75 after its normal shutdown, both pages show
  "Restarting…", then reconnect and drop the notice by themselves when the
  server is started again; not replayed after that start; a recording in
  progress (fake 8091 stream: IQ recording + DoA recording) is finished
  before the command (log order: requested, `Recorder: ... finished
  (stopped)`, files written, command; complete `.sigmf-data` + valid
  `.sigmf-meta`; the pages get `"recording":{"running":false}`); a debug
  playback test transmission whose `commands` contain `POWER:shutdown`:
  refused; phone layout 400x800 (buttons finger-sized, dialog and notice
  inside the screen, no sideways scroll); no JS errors.
- API token: an unauthenticated socket's `POWER:shutdown` is dropped
  (nothing logged); after `AUTH:` it is taken (2 checks).
- heimdall: `process_command` called directly (no dongles needed), 7 checks:
  refused without / with an invalid `KRAKEN_RESTART_EXIT` (1, "75x"),
  `get_status` unaffected, accepted with 75 (also `"command": "restart"`
  with a space), `global_running` cleared and `restart_exit_status` = 75.
- run.sh (a copy with `stop_stale` / `stop_session` / `stop_build_page`
  stubbed, fake apps), 14 checks: the supervisor passes
  `KRAKEN_RESTART_EXIT=75`, restarts on 75 with its own message, keeps the
  crash path for other statuses; headless: heimdall's 75 restarts the whole
  stack, the client is stopped and started again, logs kept, a later clean
  exit ends it with 0.
- The existing suites against this build: sidebar 53/53, page 35/35.
- `sudoers` line: `visudo -cf` "parsed OK" (run as the user on the file, no
  sudo).

Not tested: anything with real privileges (no real `systemctl poweroff` /
`reboot`, no sudo rule installed, `install-pi-service.sh` not run), a
Raspberry Pi, the boot service, Docker (compose file and restart policy
reviewed only), Windows (`shutdown.exe`, the launcher's supervisor and the
SeShutdownPrivilege check are compile-checked nowhere - no MinGW on the dev
box) and macOS (its probe branch only syntax-checked by forcing it on
Linux), heimdall's restart end to end with dongles (its command is
unit-tested, its shutdown path is the existing one).
