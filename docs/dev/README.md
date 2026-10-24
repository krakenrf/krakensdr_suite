# Developer notes

Detailed notes that used to live in the root `CLAUDE.md` (split out
2026-10-24 so that file stays short). They are not loaded into an AI
session automatically; read the one for the area you are changing.

| File | Contents |
|---|---|
| `suite-reference.md` | Long-form overview: architecture, configuration, every client feature in summary, operating modes and hardware variants, key algorithms, dependencies, testing, performance |
| `run-and-deploy.md` | run.sh / install.sh behaviour, Docker, logging, network access checks, git secret guards |
| `windows.md` | Windows port: launcher, urgent stops, compat layer, build flags, AI assistant limits |
| `librtlsdr-fork.md` | The krakenrf/librtlsdr fork: WinUSB fixes, fast retunes, USB transfer counts and measurements |
| `macos.md` | macOS port |
| `compiler-flags.md` | Compiler flags of every component and the measurements behind them |
| `licensing.md` | Licence of each part, the GPL rules, audits, clean-room rewrites |

Each app has the same layout: `heimdall_v2/CLAUDE.md` and
`kraken_doa_v2/CLAUDE.md` hold its rules, and `heimdall_v2/docs/dev/` and
`kraken_doa_v2/docs/dev/` hold one file per topic (split out the same day).
These notes still cite topics as "`kraken_doa_v2/CLAUDE.md` *Topic*" or
"`heimdall_v2/CLAUDE.md` *Topic*": look the name up in that folder's
`docs/dev/README.md`. User documentation: `README.md`, `DOCKER.md`,
`WINDOWS.md`.
