# Licensing (developer notes)

Moved verbatim from the root `CLAUDE.md` on 2026-10-24 (that file now keeps only
what every session needs). Not loaded automatically - read it when working in
this area. Where this and the component `CLAUDE.md` files disagree, the code and
the component files win.

The binding text is `LICENSE` (root) and each part's own LICENSE file.

## Licensing (LICENSE)

Set up 2026-10-21 (the user's request): the root `LICENSE` maps the parts.
- `heimdall_v2/`, `gr-krakensdr_v2/`: GPL-3.0-or-later (`LICENSE` = the GPL
  text; GPL-2.0-only is impossible - uWebSockets is Apache-2.0)
- `kraken_doa_v2/` incl. its plugins and `plugins/lib`: the
  KrakenSDR Source Available Licence 1.0 (`kraken_doa_v2/LICENSE`): free for
  personal / hobbyist use and EVALUATION (2026-10-23, the user's request:
  an organisation may test it free for 180 days - the user's call, it was
  90 - trials, test / known transmitters, side-by-side comparisons - as long
  as nothing depends on it; acting on / relying on its results, providing a
  service with it, or going past the 180 days = "deployment" = Commercial
  Use, item (f)), commercial use
  needs a licence from KrakenRF (thekraken@krakenrf.com) that includes
  `kraken_doa_v2/COMMERCIAL_TERMS.md` (the B2B terms: 90-day limited
  warranty - fix / replace / refund -, as-is otherwise, no safety-critical
  use, liability capped at 12 months' fees + no consequential loss, CGA s 43
  / FTA s 5D contracting out, ACL limit, GPL parts' rights kept, no IP
  indemnity unless agreed; law + courts as the order names - KrakenRF to
  decide). The licence's section 13 keeps non-excludable consumer law and
  contracts business use out of the CGA; contributions grant KrakenRF relicensing rights
  (its section 5, for every part - root LICENSE). Root docs: the same
  licence. Root build / run / packaging scripts, `docker/`, `windows/`,
  `scripts/`, `.githooks/`: MIT (they are part of heimdall's GPL
  corresponding source)
- MIT since 2026-10-22: the plugin SDK (`plugins/sdk/` incl. its tools,
  `plugins/SDK.md`) and the Windows helpers (`win/` - win_init.cpp,
  posix_compat.h, app.rc, app.manifest - in BOTH apps, still identical) -
  `plugins/sdk/LICENSE`, an SPDX line in each file. So a plugin of any
  licence can link them
- PER-PLUGIN LICENCES (2026-10-22, the user's request): a plugin folder with
  a `LICENSE` file is under THAT licence - a separate program (own process,
  the pipe protocol of kraken_plugin.hpp). `plugins/Makefile`
  (`own_licence` / `plugin_cxxflags` / `plugin_lib` / `licence_check`): built
  without `-Ilib` / `-I../include`, linked with pffft's objects instead of
  `libkrakendig.a`, and the build stops when `-MM` lists a header outside its
  folder, `sdk/`, `../external/pffft/`, `../win/` (realpath-normalised, so
  `../lib/x.hpp` is caught); its C++ generators the same (Windows prebuild
  rule, `make_testsignal.py`). `scripts/collect-licenses.sh` copies
  `plugins/sdk/LICENSE` + every `plugins/<id>/LICENSE` and lists the GPL
  ones in SOURCE.txt. The AI assistant may not edit `plugins/*/LICENSE*`
  (deny rule + its system prompt). Docs: root LICENSE, kraken_doa_v2/LICENSE
  section 6, THIRD_PARTY_NOTICES 1a, SDK.md *Licensing a plugin*, README.
  `plugins/ism` = GPL-2.0-or-later (it follows rtl_433's device decoders;
  every file names the rtl_433 decoders it follows + their authors, README
  *Licence*) - the first such plugin. `plugins/flarm` = GPL-3.0 (2026-10-23,
  the user's call): its OGN LDPC matrix is the one in the OGN project's
  glidernet/diy-tracker (GPL-3.0; LDPC_ParityCheck_n208k160 identical, 48 / 48
  rows, committed there by its author Pawel Jalocha himself, after the
  licence was added) - `ogn_ldpc_matrix.hpp` GPL-3.0-only, KrakenRF's files
  GPL-3.0-or-later. GPL-3.0 code (diy-tracker, SoftRF) may be used in flarm
  now, with its notices; the author's unlicensed tracker repos still not
- RULE for kraken_doa_v2: no GPL / AGPL / LGPL code in it or linked into it
  (LGPL only as a dynamic system library), and nothing TRANSLATED from GPL
  code (rtl_433, dump1090 forks, readsb, rs1729/RS, OP25, osmo-tetra,
  gr-lora*, srsRAN, ...) - such projects are documentation of FACTS and test
  references only; write from the standards / measurements. The ONE
  exception: a plugin folder with its own `LICENSE` that the GPL code's
  licence allows (above) - and nothing of `plugins/lib` / kraken_doa may go
  into such a plugin. Code ported from
  permissive code keeps its full notice in the file and gets a row (+ the
  licence text) in `kraken_doa_v2/THIRD_PARTY_NOTICES.md`
- The KrakenSDR Android app (krakensdr_rdf_android) is KrakenRF's own (the
  user, 2026-10-22): its log formats and DF grid ideas, used by mobile DF and
  the log replay, need no notice
- A new dependency: a row in the app's `THIRD_PARTY_NOTICES.md`; if binaries
  contain it, its licence file in `scripts/collect-licenses.sh` (run by
  `windows/package.sh` -> `licenses/` and the Dockerfile ->
  `/usr/share/doc/krakensdr/`, with `SOURCE.txt` = where the GPL programs'
  source is)
- Audit 2026-10-21, both findings settled 2026-10-22: `plugins/ism`
  (follows rtl_433's decoders one by one) = GPL-2.0-or-later, its own licence
  (above); `cells/lte_tables.hpp` (was srsRAN's AGPL transcription of the
  3GPP tables) is written by `cells/tools/lte_tables_gen.py` from the 3GPP
  Word files themselves (TS 36.212 V19.3.0 / 36.213 V19.5.0, every entry
  equal in the Release 8 versions; srsRAN's values were right - its
  "corrected" TBS[6][0] 88 back to the standard's 328). Also done in the audit:
  p25's NID BCH encoder (was Morelos-Zaragoza's non-commercial bch3.c form)
  and the TETRA scrambler / interleaver / depuncturer (osmo-tetra's form)
  rewritten from the standards - bit-identical output (all 65536 NIDs,
  random TETRA blocks)
- Audit of main's additions (2026-10-23, merged into worktree-licensing;
  token-level similarity against the open-source decoders + table checks,
  job scratch): `analog_id` (MDC1200 / FleetSync / selcall / DTMF / 1750 Hz)
  - its FleetSync block check (+ the generator's) restated Matthew Kaufman's
  GPL-2.0 `_fsync_crc` and the payload / FleetSync II assembly echoed its
  structure: rewritten as the MPT 1327 codeword check (the mpt1327 plugin's)
  and with arithmetic of their own, bit-identical results (2.5 M words, 6000
  generator streams, both catalogue scenes + 4 FleetSync variants); MDC1200,
  selcall, DTMF, bursts = own / facts. `rds`: no GPL code; redsea (MIT /
  ISC) behind its tables + call-sign rules got its full notices, a
  THIRD_PARTY_NOTICES row + text I and credits for two design ideas. Map
  search: Nominatim on Enter only (its usage policy), credit linked like the
  other OSM credits
- CLEAN-ROOM REWRITES (2026-10-22, the user's request): `radiosonde` v2.0
  (v1.x had code translated from rs1729/RS) and `flarm` v2.0 (v1.0 was built
  on the unlicensed OGN tracker's source) - spec writer (old code + permitted
  sources, no code in the spec) -> a separate implementer (spec only) -> a
  similarity audit vs the old plugin and the third-party code: none of their
  expression. Remaining third-party content, documented: radiosonde follows
  ra-firmware's metrology (BSD-3 notice in rs41.cpp / modem.cpp + notices
  text H) and keeps some facts the rs1729 project first published (numbers,
  layouts, rules - no code; its README lists them); flarm's
  `ogn_ldpc_matrix.hpp` (the OGN protocol's LDPC matrix, first taken from
  the unlicensed OGN tracker) is settled since 2026-10-23: the same matrix
  under GPL-3.0 from glidernet/diy-tracker, flarm a GPL-3.0 plugin (above).
  kraken_doa_v2/CLAUDE.md *Radiosonde* / *FLARM / OGN* say how;
  the specs + audits were job scratch, not in the repo

