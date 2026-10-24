# AGENTS.md

Guidance for AI coding agents working in `kraken_doa_v2/` (the KrakenSDR DoA
client). The full rules are in the CLAUDE.md files; they apply to every
agent, not only Claude. Read them before changing code:

- `../CLAUDE.md`: suite-wide rules (portability, licensing, secrets,
  numerics, logging) and the build / run commands.
- `CLAUDE.md`: kraken_doa's rules, the command path, how changes are
  tested, and an index of the feature notes.
- `docs/dev/<topic>.md`: one file per feature (design, decisions, earlier
  bugs). Read the one for the feature you are changing.

## The essentials

- Build with `make` (also builds liquid-dsp and every decoder plugin). On a
  Raspberry Pi use at most `-j3`: `make -j` or `-j4` can crash it.
  `make test-build` runs the quick source and dependency checks.
- `kraken_doa.html` is read at runtime: edit it and reload the browser, no
  rebuild.
- Control commands run only on the uWebSockets loop thread; from any other
  thread, post them with `loop->defer`.
- No GPL / AGPL / LGPL code in `kraken_doa_v2/`, and nothing translated from
  it (a plugin folder with its own `LICENSE` is the one exception - see
  `../CLAUDE.md`).
- Never commit `doa_settings.json`, `api_token`, `server.key` /
  `server.crt`, voice codecs, or binaries. The git pre-push hook checks for
  them once `../install.sh` has registered it.
- Treat `uWebSockets/`, `liquid-dsp/` and `external/pffft/` as read-only:
  they are pinned or vendored.

## Code style

- C++20, 4-space indentation, PascalCase types, snake_case functions and
  variables. Match the surrounding code's idiom and comment density.
- Protect shared state with the existing mutexes and atomics; prefer RAII.
- Hz, epoch milliseconds, byte and sample counts are 64-bit (`long` is
  32-bit on Windows).

## Testing

There is no automated suite in the repo. Follow `CLAUDE.md` *Testing a
change*: a test build on its own ports and in its own folder, a fake
heimdall that streams synthetic IQ, and headless Chromium with real mouse
and key events for the page. Never test against the live receiver's ports
or settings.

## Commits

Subject `area: what changed`, in the present tense (for example `pagers:
FLEX pages added to the POCSAG plugin`, `Web UI: Safari fixes - decoder tab
strip`); the body says why and how the change was checked. Include
screenshots or CPU / latency figures for UI or performance changes.
