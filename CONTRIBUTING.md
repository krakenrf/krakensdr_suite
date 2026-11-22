# Contributing to the KrakenSDR suite

Fixes, features, decoder plugins and documentation are welcome as pull
requests to https://github.com/krakenrf/krakensdr_suite. Before you send one,
read the contribution terms below: a pull request is merged only when its
author has agreed to them.

## Contribution terms

The suite is made of parts under different licences ([LICENSE](LICENSE)).
The KrakenSDR DoA software (`kraken_doa_v2/`) is source-available: free for
personal and hobbyist use, while companies and other organisations buy a
commercial licence from KrakenRF Inc. Code also moves between the parts. So
KrakenRF needs more rights in a contribution than the licence of the part
gives to everybody.

The binding text is the *Contributions* section of [LICENSE](LICENSE) and
section 5 of the
[KrakenSDR Source Available Licence](kraken_doa_v2/LICENSE). In short, for a
contribution to any part of the suite - also the GPL programs, the MIT parts
and the plugins with a licence of their own:

1. You license your contribution under the licence of the part it changes
   (the table below).
2. You also grant KrakenRF Inc. and its successors a perpetual, worldwide,
   non-exclusive, royalty-free, irrevocable licence to use, copy, modify,
   distribute and sublicense your contribution, and works based on it, under
   any terms. That includes the commercial licences and products KrakenRF
   sells, without payment to you, and open source licences. The same holds
   for any patent you hold that the contribution needs.
3. You confirm that the contribution is your own original work, or that you
   otherwise have the right to grant these licences - if your employer owns
   what you write, you have its permission - and that you have named all
   third-party material in it and the terms that material is under.

You keep the copyright in your contribution and can use it elsewhere as you
like. KrakenRF is not obliged to merge a contribution.

If you cannot agree to these terms, do not send the contribution. Section 5
allows other terms when they are agreed in writing: contact
thekraken@krakenrf.com first.

## How you agree

**A pull request.** Its description starts from a template with this line.
Tick the box and leave the sentence as it is:

```
- [x] I agree to the KrakenSDR contribution terms (CONTRIBUTING.md): I license my contribution under the licence of the part of the suite it changes, and I grant KrakenRF Inc. the rights in section 5 of the KrakenSDR Source Available Licence - KrakenRF may use, copy, modify, distribute and sublicense it under any terms, including in commercial products and licences it sells, without payment to me. It is my own work, or I have the right to contribute it on these terms, and this pull request names any third-party material in it and its licence.
```

The *Contribution terms* check on the pull request fails until the line is
there with its box ticked. A pull request opened before the template existed
gets the line by editing its description.

**Commits by somebody else.** The line covers what you wrote yourself. When
the pull request holds commits by another person (GitHub shows them as the
commit's author), that person adds a comment to the pull request with the
sentence above ("I agree ... its licence."). The check then has to run
again: edit the description, push, or ask a maintainer to re-run it.

**A patch by e-mail, or a file attached to an issue.** The same terms apply
(section 5 names these too). Put the sentence into your message.

## What a contribution may contain

Name every piece of third-party code, data or other material, and its
licence, in the pull request - the template has a line for it.

| Part | Its licence | Third-party material it can take |
|---|---|---|
| `kraken_doa_v2/`, including `plugins/` and `plugins/lib/` | KrakenSDR Source Available Licence | Only under a permissive licence (MIT, BSD, ISC, Apache-2.0, zlib, public domain / CC0), with its full notice kept in the file and a row in `kraken_doa_v2/THIRD_PARTY_NOTICES.md` |
| A plugin folder with a `LICENSE` file of its own (`plugins/ism`: GPL-2.0-or-later, `plugins/flarm`: GPL-3.0) | The licence in that folder | What that licence allows, with its notices kept. Never code from `plugins/lib/` or from the rest of kraken_doa |
| `heimdall_v2/`, `gr-krakensdr_v2/` | GPL-3.0-or-later | Under a licence compatible with GPL-3.0, with a row in `heimdall_v2/THIRD_PARTY_NOTICES.md` |
| `kraken_doa_v2/plugins/sdk/`, `plugins/SDK.md`, `win/`, `mac/`; the scripts in the repository root and in `docker/`, `windows/`, `scripts/`, `.githooks/`, `.github/` | MIT | Only under a permissive licence |
| Documentation in the repository root and in `docs/` | KrakenSDR Source Available Licence | - |

- **No GPL, AGPL or LGPL code in `kraken_doa_v2/`**, and nothing translated
  or adapted from such a project (rtl_433, dump1090 forks, readsb,
  rs1729/RS, OP25, osmo-tetra, gr-lora, srsRAN, SDRTrunk and the like).
  KrakenRF could not license it commercially. Use those projects as
  documentation of facts and as test references only; write the code from
  the standards and from measurements. Code that does follow a GPL project
  belongs in a plugin folder with its own `LICENSE`
  (`kraken_doa_v2/plugins/SDK.md`, *Licensing a plugin*).
- **Never in a pull request**: voice codecs (ETSI TETRA, AMBE / mbelib,
  libfdk-aac); `*.exe`, `*.dll` and other built binaries;
  `doa_settings.json`, `api_token`, `server.key`, API keys, your station's
  coordinates; real people's messages (pager pages and the like).
- **Code written with an AI tool** is your contribution like the rest: make
  sure that it does not reproduce third-party code and that you have the
  right to license it on these terms.

## Before you open the pull request

- `./install.sh` once per clone: it registers the git filter and the
  pre-push hook that keep station secrets and codecs out of a push.
- Build what you changed (`make -C heimdall_v2`, `make -C kraken_doa_v2`,
  `make -C kraken_doa_v2/plugins PLUGIN=<id>`) and try it on the hardware or
  with a recording. A Windows or macOS change must leave the Linux build as
  it was.
- The rules of each part are in `CLAUDE.md`, `heimdall_v2/CLAUDE.md` and
  `kraken_doa_v2/CLAUDE.md` (they apply to people as much as to AI tools),
  the design of each feature in the `docs/dev/` folders, the plugin guide in
  `kraken_doa_v2/plugins/SDK.md`.
- Say in the description what the change does, why, and how you tested it.
