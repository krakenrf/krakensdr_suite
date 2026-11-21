# Version indicator

Added 2026-11-16 at the user's request: the web UIs show which code runs, and
the version changes by itself with every commit / push - nothing is bumped by
hand. There are no tags or version numbers in the repository; the version is
derived from git.

## The text

`r<commits on HEAD> · <abbreviated hash>`, e.g. `r294 · 63a4917`. The count
orders versions (a higher r is newer on the same line of history), the hash
identifies the commit exactly. Variants:

- `· modified` appended: tracked files differ from the commit (staged or not;
  untracked files don't count).
- A shallow clone (`git clone --depth N`) counts only the commits it has, so
  there the count is replaced by the commit date: `2026-11-16 · 63a4917`.
- Not known at all: the pages show `Version unknown`; `--version` prints
  `unknown`.

The details - commit date (ISO 8601 with offset), branch (none on a detached
HEAD), where the answer came from, and the program's compiled-in version when
it differs (the binary was built from another commit than the tree it runs in,
e.g. after a `git pull` that changed only a page, which `run.sh` doesn't
rebuild for) - are in the tooltip and in `--version`'s second line.

Same branch, different counts are fine; two branches can have the same count
(the hash tells them apart). Merges count every commit reachable from HEAD.

## Where it comes from (first that answers)

1. **git** - the work tree the program runs in (its cwd; run.sh starts each
   app in its folder of the clone). Only if `git rev-parse --show-toplevel`
   has `heimdall_v2/` and `kraken_doa_v2/` (a data folder inside some other
   project is not the suite). Four git calls: `rev-parse --show-toplevel
   --absolute-git-dir --is-shallow-repository --abbrev-ref HEAD`, `log -1
   --abbrev=7 --format=%h%n%cI`, `rev-list --count HEAD` (not on a shallow
   clone) and `--no-optional-locks diff --no-ext-diff --name-only
   --ignore-submodules=all HEAD --` for "modified".
2. **file** - `./VERSION` (cwd-relative, like every runtime file): written by
   the Docker build and `windows/package.sh`, which have no git at run time.
3. **build** - compiled in: both Makefiles run `scripts/version.sh --header`
   on every make into `build/build_version.h` (heimdall) /
   `obj/build_version.h` (kraken_doa), rewritten only when it changes, so a new
   commit recompiles `version_info.cpp` alone and relinks. That object has its
   own rule (`-I` of the object folder for that file only; a target-specific
   variable would have reached the flags stamp, which other prerequisites
   share). heimdall's CMakeLists has no compiled-in version.
4. **unknown**.

Formats: `VERSION`, the script's output and the compiled-in string are the
same `key=value` lines (`count`, `hash`, `date`, `branch`, `modified`,
`shallow`); the programs build the text from them (`appver::format_text`), so
the format exists once. The script and `version_info.cpp` ask git the same
questions - keep them alike.

### "modified" and the clean filter

`git status` is not used: for a file with a clean filter (`.gitattributes`:
`doa_settings.json`, the web mapper's config - secrets blanked by
`scripts/git-secrets-clean`) it trusts a size change and reports ` M` although
the filtered content equals the commit (git's `ce_modified` shortcut;
measured: real coordinates of another length on disk -> ` M`, `git diff`
empty). `git diff --name-only HEAD` compares the filtered content, refreshes
stat-only changes in memory (a touched file is not modified) and with
`--no-optional-locks` never writes the index or takes `index.lock` (so it
can't race the user's own git commands). No tracked file has the filter today;
it matters if one is ever force-added.

## Runtime: refresh without blocking

`appver::start()` (both apps, after the log setup) runs a background thread:
it resolves once and logs `Version: r294 · 63a4917 (git, branch main,
2026-11-16T00:23:16+13:00)` to stdout, then every 10 s compares the mtime and
size of `<git dir>/HEAD`, `index` and `logs/HEAD` - a commit, pull, checkout,
reset or the user's `git status` changes one of them - and resolves again on a
change, and anyway every 5 minutes (an edited file, which touches none of
them). A changed answer is logged as `Version now: ...`. Readers
(`appver::json()`, the status builders) only copy a cached string under a
mutex: never git on the uWS loop or in a status broadcast. git calls time out
(5 s; the diff 15 s, a Pi's SD card) and are killed.

Processes: kraken_doa spawns git through `kproc::run_capture` (search_path:
PATH only, never the cwd on Windows; `Console::None`). heimdall has its own
small `core/run_capture` (posix_spawnp, stdin / stderr to /dev/null,
close-on-exec pipe + glibc's `closefrom` file action; Windows: CreateProcessW
with an explicit inherited-handle list, PATH lookup of `git.exe`, PeekNamedPipe
polling, TerminateProcess at the timeout). No shell anywhere.

Shared code: `version_info.hpp` / `.cpp`, identical in
`heimdall_v2/src/core/` and `kraken_doa_v2/include/utils/` +
`kraken_doa_v2/src/utils/` (only the `#include` line differs), MIT (SPDX line)
so the same text can live in the GPL and the source-available app. The runner
is injected (`appver::set_runner`).

## Docker and the Windows package

- **Docker**: the build context now includes `.git/` and
  `scripts/version.sh` (`.dockerignore`). After the app builds, the build stage
  copies them (`COPY scripts/version.sh .gi[t] /src/gitmeta/` - the pattern
  lets a context without `.git` build) and runs the script with
  `GIT_DIR=/src/gitmeta GIT_WORK_TREE=/src --ignore-missing` (the context holds
  only part of the tree; files missing from it are not changes:
  `--diff-filter=d`) into `/src/VERSION`, then deletes the git metadata. The
  runtime stage copies `VERSION` next to both apps and the entrypoint links it
  into `/data/heimdall` and `/data/kraken_doa`. The `.git` copy is after the
  builds, so a new commit re-runs only that step and the VERSION copies; it
  never reaches the runtime image. A build from a git worktree (`.git` is a
  file) or a source archive gives an empty VERSION: "Version unknown". The
  Makefiles find no script in the image build, so the image has no
  compiled-in version.
- **Windows package**: `windows/package.sh` writes `VERSION` into
  `heimdall_v2\` and `kraken_doa_v2\` of the package from the clone it runs in
  (empty without git: the exes' compiled-in version is shown). A Windows
  source-tree run uses git at run time like Linux, if git is on PATH.

## Where it is shown

- kraken_doa page: the last line of the sidebar's footer box (after the
  layout buttons; `#sb-version`, `sbVersionRender`), small and muted, the
  details in its `title`. From `system_status.version` (every 500 ms; the DOM
  is touched only when it changed). Hidden while empty - an older receiver
  that doesn't send the field shows nothing.
- kraken_doa also passes heimdall's version on: it reads the `version` object
  of heimdall's 2 Hz control-port status (`app_version.cpp`, its own line
  splitter on the drained 8092 bytes; only the top-level key; the fields
  re-checked and the text rebuilt), sends it as `system_status.heimdall_version`
  (dropped after 30 s without one), and the tooltip says `heimdall: the same`,
  `heimdall r290 · abcdef1` or `heimdall: version not known`.
- heimdall page: `heimdall r294 · 63a4917` at the right of the top bar
  (`#version-line`; the last line of the page until 2026-11-18), details in
  its `title`, from the web `STATE` message.
- heimdall 8092: `version` in the status broadcast and the `get_status` reply
  (`gr-krakensdr_v2/docs/heimdall_v2_control_protocol.md`).
- `--version` on both programs: the text, then a detail line; exit 0.
  (kraken_doa prints `RawDataBuffer initialized: ...` before it - a static
  initialiser that runs before `main`, as with `--help`.)
- The startup log (stdout): `Version: ...` once.

## Tests (2026-11-16)

x86_64 Ubuntu 22.04 (git 2.34), TEST builds of both apps (ports 2828x / 2829x),
headless Chromium over the DevTools protocol.

- Fallback order with `--version` (both programs): no git tree / no VERSION /
  no compiled-in -> `unknown`; a `VERSION` file -> `from file`; a garbage
  VERSION -> skipped; compiled-in (Makefile header) -> `from build`; git work
  tree -> `from git`, also from a subfolder, and before a VERSION file. The
  Makefile header: written on the first make, only `version_info.cpp`
  recompiled; a second make does nothing.
- 27 runtime-git checks in throwaway clones: count + hash; branch; date;
  detached HEAD (no branch); untracked file -> not modified; a changed tracked
  file and a staged change -> modified; touched file, same content -> not
  modified; a new commit -> r+1 and the new hash; the clean filter configured
  as install.sh does, a force-added settings file: real secrets on disk and
  coordinates of another length (git status says ` M`) -> not modified, a real
  change -> modified; shallow clone (depth 3) -> date form, "shallow clone"
  noted, the script prints no count; another repository -> ignored; no git on
  PATH -> compiled-in; the script and the programs agree.
- Page, 30 checks: the line shows the git text, visible, last line of the
  footer box after `.sb-layout`; tooltip (date, branch, source, "program built
  from" a different compiled-in version); heimdall's version from a fake 8092
  (lines split across writes, a nested "version" key ignored): differs / the
  same / unknown / an older heimdall (gone after 30 s); a commit in the clone
  shows in the open page in ~5 s with no rebuild or reload; a local edit ->
  `· modified` after a `git status` (which rewrites the index), gone again
  after undoing it; an older server (field removed) -> line empty and hidden,
  no JS errors; unknown / malformed / shallow / VERSION-file objects; phone
  width (390 px): inside the screen, no sideways scroll; no JS errors. The
  sidebar suite (53) and the page suite (35) pass unchanged.
- heimdall end to end, 11 checks (a KrakenSDR was attached and free on the
  test PC; the TEST heimdall ran < 1 min on ports 2828x / 2829x): 8092
  `get_status` and the broadcast carry the version; heimdall's page shows
  `heimdall r295 · 7581f6e` with its tooltip, an older heimdall (field
  removed) leaves it hidden; kraken_doa connected to it shows `heimdall: the
  same`; one `Version:` line in heimdall's startup log.
- Docker: the Dockerfile's version step simulated without Docker (no Docker on
  the test PC): a context copy with `.git` -> the VERSION file, a context
  without `.git` / with a worktree `.git` file -> empty VERSION. The image
  itself was not built.
- Not tested: a Pi (timing of the diff on an SD card), the Windows build
  (no MinGW on the test PC - `run_capture.cpp`'s Windows part is unverified),
  macOS, a Docker image build.
