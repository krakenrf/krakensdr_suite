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

The component files `heimdall_v2/CLAUDE.md` and `kraken_doa_v2/CLAUDE.md`
hold the details of each app. User documentation: `README.md`, `DOCKER.md`,
`WINDOWS.md`.
