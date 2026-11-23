# HTTPS certificate of the web UI (port 8080)

The page is HTTPS only (the browser needs a secure page for audio and the
microphone). Since the PR #5 follow-up (2026-11-23) each receiver has its
own small certificate authority (CA): users install the CA once per device,
and the server certificate it signs can then be re-issued at any time (new
address, expiry, update) without a warning or a new install. PR #5's first
version (one self-signed certificate, renewed in place) made everyone trust
it again after every renewal and rename, broke on OpenSSL 1.1.1 (two Basic
Constraints extensions: `ERR_CERT_INVALID`, no way to click through), never
picked up a new IP and overwrote users' own certificates.

This file is the contract `scripts/make-cert.sh` (Linux / macOS / Docker),
kraken_doa's in-process generator (`src/utils/tls_compat.cpp`, Windows and
any platform without the openssl CLI) and the page share. Sections marked
*(contract)* are fixed; change them in all three places at once.

## Files *(contract)*

In kraken_doa's working directory (`kraken_doa_v2/`, `/data/kraken_doa` in
Docker). All gitignored; only the CA certificate is ever served.

| File | What | Mode |
|---|---|---|
| `krakensdr-ca.key` | the receiver's CA key (RSA 3072) | 0600 |
| `krakensdr-ca.crt` | the CA certificate - what users install | 0644 |
| `server.key` | the server key (RSA 2048): created once, KEPT for every re-issue (the planned receiver mesh pins it; native apps may pin it) | 0600 |
| `server.crt` | the server certificate followed by the CA certificate (uSockets loads it with `SSL_CTX_use_certificate_chain_file`) | 0644 |
| `cert_options` | one line `scope=narrow` or `scope=wide`; written by kraken_doa (`CERT_SCOPE:`), read at the next start; missing = narrow | 0644 |
| `krakensdr-ca.YYYYMMDD.retired.crt` | a replaced CA's certificate (its key deleted) - lets the page name the CA devices should remove | 0644 |

## Names *(contract)*

- `<host>`: the name avahi announces (it differs from `hostname` after an
  mDNS name conflict: the second `krakensdr` becomes `krakensdr-2`), else
  `hostname -s`; on macOS `scutil --get LocalHostName`; Windows
  `gethostname()`. `KRAKEN_CERT_HOST` overrides (Docker). Lowercased; a
  character outside `a-z 0-9 -` makes the name unusable for DNS - then only
  `localhost` and (wide scope) IPs are named, and the page says why.
- The DNS names: `<host>`, `<host>.local`, `<host>.<d>` for each `search` /
  `domain` entry of `/etc/resolv.conf` (not `local`), `localhost`, and each
  name in `KRAKEN_CERT_NAMES` (comma-separated, added).

## Scope *(contract)*

| | narrow (default) | wide (opt-in on the page) |
|---|---|---|
| CA name constraints, DNS | exactly the names above | the names above, plus every name under `local` and `lan` |
| CA name constraints, IP | `127.0.0.1/32`, `::1/128` only | `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, `100.64.0.0/10`, `127.0.0.0/8`, `fc00::/7`, `::1/128` |
| server certificate's IPs | `127.0.0.1`, `::1` | those plus the current global IPv4 addresses |
| opened by IP | the usual warning (click through) | trusted while the address is in a private range |
| risk if the CA key leaks | it can vouch only for this receiver's names, and only as a TLS server (Fine rule 16) | it can impersonate any `.local` / `.lan` name and any private IP (router, NAS) as a TLS server to devices that installed it |

An IP permitted subtree is required in narrow scope: with no iPAddress
entry at all, RFC 5280 leaves IP names UNconstrained. Every name in the
server certificate must lie inside the CA's constraints - one outside fails
the whole certificate, including its other names.

## The CA *(contract)*

- Subject `CN=KrakenSDR <host> CA <YYYY-MM-DD>, O=KrakenSDR receiver`
  (the date tells two CAs apart on a device).
- `basicConstraints = critical, CA:TRUE, pathlen:0`,
  `keyUsage = critical, keyCertSign, cRLSign`, subject key identifier,
  `nameConstraints = critical, ...` per scope. Validity 3650 days.
- A NEW CA (new key; the old certificate renamed `.retired`, its key
  deleted) when: there is none; a wanted DNS name is outside its
  constraints (a rename, an avahi conflict name, a new search domain); the
  scope in `cert_options` differs from the CA's; it expires within 365
  days; `--new-ca`. Every device must then install the new one.
- Optional: the same CA on several receivers - copy `krakensdr-ca.key` /
  `.crt` from one to the others before their first start (or replace and
  `--force`); one install then covers them all, as long as each receiver's
  names are inside its constraints (wide scope, or `KRAKEN_CERT_NAMES` on
  the first).

## The server certificate *(contract)*

- Subject `CN=<host>, O=KrakenSDR receiver` (`CN=localhost` when there is no
  usable host name); `basicConstraints = critical, CA:FALSE`,
  `keyUsage = critical, digitalSignature, keyEncipherment`,
  `extendedKeyUsage = serverAuth`, authority / subject key identifiers,
  `subjectAltName` = the names + IPs per scope. Validity 825 days (the most
  Apple accepts for a server certificate trusted through a user-installed
  CA).
- Re-issued with the SAME key when: its names / IPs differ from the wanted
  ones; fewer than 60 days left (or expired); it does not verify against the
  current CA; `--force`.

## Managed or not *(contract)*

- Managed: `server.crt` verifies against `krakensdr-ca.crt`; or its subject
  has `O=KrakenSDR receiver` (PR #5's self-signed one); or its subject is
  exactly `CN=krakensdr` (install.sh / Docker / Windows before PR #5). An old
  one is migrated: a CA is created and the server certificate issued with
  the EXISTING `server.key`.
- Anything else - a user's own certificate - and any `server.crt` /
  `server.key` that is a symlink: never touched ("not managed").

## Fine rules *(contract, settled 2026-11-23)*

The script and the in-process generator must agree on every detail, or
each re-issues what the other made. Where the two first implementations
differed, this is the rule both follow (the orchestrator's choice; a later
change goes into both and this list):

1. **Dates in UTC**: the CA's CN date and the retired-file date.
2. **Retired CA**: `krakensdr-ca.YYYYMMDD.retired.crt`, then `-2`, `-3` ...
   by which names exist; only a readable, non-symlink CA certificate is
   retired; its key is deleted.
3. **An existing CA's scope**: wide when its permitted IP subtrees include
   `10.0.0.0/8`; narrow when it has name constraints without that; a CA
   with NO name constraints is scope `none` and is always replaced by a new
   CA (an unconstrained CA is never kept).
4. **`KRAKEN_CERT_NAMES`**: split on commas and whitespace, lowercased,
   trailing dots removed; an entry that parses as an IPv4 / IPv6 address is
   dropped (browsers never match an address against a DNS name), as is an
   invalid DNS name - each with a note (stderr / the log).
5. **`cert_options`**: lines `scope=VALUE`, spaces around `=` allowed, key
   and value case-insensitive, the last such line wins; `wide` means wide,
   anything else narrow. kraken_doa writes exactly `scope=wide` /
   `scope=narrow` and a newline.
6. **Host name**: `KRAKEN_CERT_HOST` trimmed of whitespace and lowercased,
   NOT cut at a dot (a dot makes it unusable); every other source cut at the
   first dot and lowercased. Usable = one DNS label, 1-63 of `a-z 0-9 -`,
   not starting or ending with `-`. Unusable -> `CN=localhost`, DNS names
   `localhost` + `KRAKEN_CERT_NAMES`.
7. **avahi**: busctl, then gdbus (2 s timeouts each, after avahi's GetState
   has settled, waiting at most 3 s for it), then `avahi-resolve -a` on a
   private IPv4 address accepting only a `.local` answer, then the system
   host name.
8. **Wide scope's addresses**: the IPv4 addresses of interfaces that are up,
   without `lo`, 127/8 and 169.254/16, and only those inside the CA's
   private ranges (10/8, 172.16/12, 192.168/16, 100.64/10); plus 127.0.0.1
   and ::1. Compared with the server certificate as a set.
9. **Validity**: notBefore = now - 1 h; notAfter = notBefore + 825 days
   (server) / 3650 days (CA). Re-issue below 60 days left (or expired); a
   new CA below 365 days left.
10. **Verification** for "issued by our CA" / "managed": chain and signature
    WITHOUT the time check (`-no_check_time`); time is only the rule above.
11. **An unparseable `server.crt`** (the file exists, no certificate in it):
    failure - exit 1 / an error, every file untouched.
12. **An unreadable / unparseable key** (`server.key` or `krakensdr-ca.key`):
    never deleted or overwritten.
    - Permission denied (the file belongs to another user, e.g. after
      `sudo ./run.sh`): failure - exit 1 / an error naming the file and
      "check its owner", every file untouched; never a new CA because of it.
    - Readable but not a key we can use (encrypted, garbage): if
      `server.crt` is a user's own certificate - not managed, nothing
      touched. Otherwise the file is moved aside to
      `<name>.unreadable-YYYYMMDD` (`-2`, `-3` ...; UTC) and a new key is
      made (`issued`; for the CA key `ca-created`).
13. **Modes**: `server.key` and `krakensdr-ca.key` set to 0600 on every run
    and every path (`kept`, `issued`, `ca-created`), never following a
    symlink; on Windows the owner-only ACL likewise.
14. **Lock**: `<dir>/.make-cert.lock` (a directory holding its creation time;
    wait up to 10 s; a lock older than 120 s - or dated more than 60 s in
    the FUTURE: a clock set back - is taken over by renaming it away
    atomically first, so two waiters never both take it; removed on exit)
    - taken by the script AND by the in-process generator, so a manual run,
    install.sh and kraken_doa never write at the same time.
15. **Managed - legacy subjects only when self-signed**: the `O=KrakenSDR
    receiver` and exact `CN=krakensdr` rules of "Managed or not" apply only
    to a certificate that is self-issued AND verifies with its own key (PR
    #5's, the old install.sh / Docker / Windows ones). A certificate from any
    other CA is a user's own, whatever its subject. Before a managed
    `server.crt` is ever replaced by a migration, it is copied to
    `server.crt.replaced-YYYYMMDD` (`-2` ...; 0644).
16. **The CA is limited to TLS servers**: `extendedKeyUsage = serverAuth`
    on the CA certificate too (a "technically constrained" CA: a leaked key
    cannot sign code-signing, e-mail or client certificates); an existing CA
    without it is replaced (a new CA).
17. **The CA's CN** is `KrakenSDR <host> CA <YYYY-MM-DD>` with `<host>`
    shortened to its first 40 characters (X.509 limits a CN to 64).
18. **When a new CA, when only a narrower server certificate**: a new CA
    only for the triggers of "The CA" where the wanted name is one of the
    host's OWN names (`<host>`, `<host>.local`) - a rename - and for scope,
    expiry, no / partial constraints (a CA without a DNS subtree, rule 3),
    no serverAuth (rule 16) and `--new-ca`. Other wanted names outside the
    CA's constraints (a new search domain on another network, a VPN's DNS
    suffix, `KRAKEN_CERT_NAMES` added later) are LEFT OUT of the server
    certificate and reported (status notice `names_not_covered` with the
    names; the script prints them): moving between networks must not make
    every device install a new CA. `--new-ca` (or the page, later) takes
    them in.
19. **Download**: `/krakensdr-ca.crt` serves the CA certificate RE-ENCODED
    from the parsed certificate (PEM of the first certificate only) - never
    the file's bytes, which may hold a private key.
20. **Host allow-list**: kraken_doa adds to it the served certificate's
    names that are the host's own (`<host>`, `<host>.local`, avahi's name),
    `KRAKEN_CERT_HOST`'s and `KRAKEN_CERT_NAMES`' names, and `<host>.<d>`
    only for the search domains the allow-list already trusts (lan, home,
    localdomain, home.arpa) - not an arbitrary DHCP search domain (a
    network that sets `search attacker.example` must not open DNS
    rebinding from it).
21. **Secrets everywhere**: `krakensdr-ca.key`, every `*.unreadable-*`,
    `*.key.tmp`, the script's `.make-cert.*` folders are gitignored, blocked
    by `.githooks/pre-push`, and denied to the AI assistant
    (`kraken_doa_v2/ai/kraken_ai.py` `SECRETS`, its sandbox's denyRead).

## make-cert.sh *(contract)*

`scripts/make-cert.sh <dir> [--force] [--new-ca]` - exit 0 when a usable
pair is there; the last line of output is one of:

| Last line | Meaning | run.sh says |
|---|---|---|
| `kept` | nothing changed | nothing |
| `issued` | server certificate (re-)issued, CA unchanged: devices that installed the CA need nothing | an info line |
| `ca-created` | a new CA: every device must install it (and remove the retired one) | a warning |
| `not-managed` | a user's own certificate, left alone | an info line |

Failure: exit 1, a reason on stderr, the existing files untouched. Bash 3.2
(macOS), OpenSSL 1.1.1 / 3.x / LibreSSL: own config files (`-config` /
`-extfile`), never `-addext` on top of the system's `openssl.cnf`; names read
back with `x509 -text`, not `x509 -ext`.

## kraken_doa *(contract)*

- `GET /krakensdr-ca.crt` (and `/krakensdr.crt`, same response): the CA
  certificate, `application/x-x509-ca-cert`, no token (needed before the page
  can be trusted); without a CA (not managed) the server certificate's first
  certificate.
- `system_status.cert` (in the 2 Hz status, only fields that changed cost
  nothing extra - or on change): `{managed, scope, scope_next, ca: {name,
  sha256, not_after}, server: {names: [...], ips: [...], not_after,
  days_left}, notices: [...]}` with notices `expires_soon` (< 30 days),
  `name_changed` (the host name is not in the served certificate),
  `addresses_changed` (wide scope: a current IPv4 address is not in it),
  `restart_pending` (scope changed, applied at the next start).
- `CERT_SCOPE:narrow|wide` writes `cert_options`; refused during a playback;
  the page shows the choice, the risk (help tip) and "applies at the next
  start; every device then installs the new certificate".
- The in-process generator follows every rule above (same files, names,
  scope, triggers, managed check).

## Native apps

Pin the server key (SHA-256 of its SubjectPublicKeyInfo: it stays the same
across re-issues) or the CA certificate - never the server certificate's
fingerprint, which changes with every re-issue (`NATIVE_APP_API.md`).

## make-cert.sh: how it applies the contract

Details the contract leaves open. The script and kraken_doa's generator
(`tlscert::ensure`) settle them alike - each finds the other's files
`kept` (cross-tested, see *Tested*); change both together.

- Host name lookup on Linux: avahi over D-Bus first (`busctl call
  org.freedesktop.Avahi / org.freedesktop.Avahi.Server GetHostName`, after
  waiting up to 3 s while `GetState` says registering / collision), then
  `gdbus` (the same call), then `avahi-resolve -a <first non-loopback IPv4>`
  - only an answer ending in `.local` counts (on the dev box it answered
  with the router's `<host>.lan` name); then `hostname -s`, `hostname`,
  `uname -n` (cut at the first dot). macOS: `scutil --get LocalHostName`.
  `KRAKEN_CERT_HOST` is trimmed and lowercased, not cut: a dot in it makes
  it unusable. Usable = a DNS label: 1-63 of `a-z 0-9 -`, not starting or
  ending with `-`. With no usable host name the names are `localhost` plus
  `KRAKEN_CERT_NAMES`, the CN `localhost`.
- A DNS name = such labels joined by dots, 253 characters at most.
  resolv.conf: every word after `search` / `domain`, lowercased, trailing
  dots removed; `local`, and what is no DNS name then (`.`, which
  systemd-resolved writes as `search .`), are skipped. `KRAKEN_CERT_NAMES`:
  split at commas only, each trimmed, lowercased, trailing dots removed; one
  that is no DNS name is left out with a note on stderr; one that looks like
  an IPv4 address is named as a DNS name, as the generator does, with a note
  (browsers don't match an address against it).
- Wide scope's IPv4 addresses (getifaddrs' view): `ip -4 -o addr show up`
  without the `lo` interface (macOS: `ifconfig -u` without loopback
  interfaces; else `hostname -I`), 127/8 and link-local 169.254/16 left out,
  and of the rest only those inside the wide constraints - 10/8, 172.16/12,
  192.168/16, 100.64/10 - so a public address never makes the whole
  certificate fail (8.8.8.8 under a wide CA: refused). `127.0.0.1` and `::1`
  are always named. `addresses_changed` uses the same filter.
- `cert_options`: the last line starting `scope=` (the line and the value
  trimmed); exactly `wide` = wide, anything else narrow (a note).
- "Names / IPs differ": the SAN set compared sorted, order-free (`::1` is
  printed as `0:0:0:0:0:0:0:1`). The CA's scope: wide when its permitted DNS
  subtrees include `local`, else narrow - also for a CA with no Name
  Constraints, which permits every name (kept in narrow scope, replaced in
  wide). A wanted DNS name is inside a permitted `DNS:c` when it is `c` or
  ends in `.c`.
- Managed: the subject rules, or the leaf verifies against
  `krakensdr-ca.crt` ignoring the time (`-no_check_time`: an expired
  certificate of ours is still ours). Not managed needs the pair: a
  `server.crt` that isn't ours (also one openssl can't read, or a symlink)
  with a `server.key` = `not-managed`; without the key = exit 1. An
  unreadable `server.key` with no `server.crt` (a passphrase?) = exit 1; with
  our certificate = a new key.
- Re-issue: also when it doesn't verify against the CA NOW (`openssl verify
  -purpose sslserver -no-CApath -no-CAstore -CAfile <ca>`: chain, validity,
  constraints, purpose - the generator's `verifies`; a clock behind the
  certificate's start re-issues it, the CA stays), when `server.crt` isn't
  exactly [leaf, current CA] (a leaf-only file), when it doesn't match
  `server.key` (the key is kept), and when `server.key` is missing or
  unreadable. Expiry by `-checkend`, read from its MESSAGE: OpenSSL 3.0.2
  (Ubuntu 22.04's) exits 0 for "Certificate will expire" too - PR #5's
  renewal check never fired there.
- Validity: notBefore = now - 1 hour (a device clock a little behind still
  accepts a certificate made just now), notAfter = notBefore + 825 / 3650
  days - as the generator. Signed with `openssl ca -startdate -enddate`
  (`-selfsign` for the CA, a throw-away database in the stage folder): the
  one way 1.1.1, 3.x and LibreSSL can all set the start. Serial numbers: 128
  random bits.
- Retiring: the old CA certificate is renamed
  `krakensdr-ca.YYYYMMDD.retired.crt` (`...-2.retired.crt`, `-3`, ... for
  a second one the same day), only if it is a readable certificate; the new
  CA key replaces the old one by rename. Both that date and the CA's CN date
  are local time, as the generator's.
- One run at a time: `<dir>/.make-cert.lock` (mkdir; it holds the time it
  was taken), waited for up to 10 s, then exit 1 ("another make-cert.sh is
  working"); one older than 120 s (or without its time after the wait) is a
  dead run's and taken over; removed on exit, also after TERM / INT. A
  read-only `<dir>` gets no lock (nothing can be written there).
- `kept` costs six openssl calls (one `x509` per certificate with every
  value at once, the two keys, one `verify`, `version`) and makes no key:
  53 ms on the x86_64 dev box (was 154 ms with 19 calls).
- Output before the last line: `reason: <text>` (issued / ca-created /
  not-managed) and `retired: <file>`; a fresh install says exactly `reason:
  first certificate` (callers word their message by it). Notes (bad
  `cert_options`, a skipped name, an unusable host name) go to stderr.
- Commit order (each a rename in `<dir>`): old CA certificate -> retired,
  new CA key, new CA certificate, new server key, `server.crt`. An
  interruption leaves a state the next run repairs (a CA key without its
  certificate -> new CA; a key without its certificate -> re-issue).
- On macOS the script prefers Homebrew's `openssl@3`
  (`/opt/homebrew/opt/openssl@3/bin/openssl`, `/usr/local/opt/...`; install.sh
  installs it) over the system's LibreSSL; `OPENSSL` overrides.
- Callers (same messages in all three): run.sh's `cert_check` at every start
  (before the old stack is stopped) AND before every start of kraken_doa -
  the supervisor's (`supervise --before cert_check`: the page's Restart, a
  crash) and headless mode's: kraken_doa only makes a pair when there is
  none, so a scope chosen on the page, a rename or a near expiry would
  otherwise wait for the next run.sh; the tmux client pane gets
  `KRAKEN_CERT_HOST` / `KRAKEN_CERT_NAMES` from run.sh (`env`, removed when
  unset - the pane has the tmux server's environment). install.sh's
  `install_kraken_certs`; the Docker entrypoint's `ensure_cert` (the script
  is copied to `/opt/krakensdr/scripts/`; `iproute2` is in the image for
  `ip`). Nothing for `kept`; an info line for `issued` / `not-managed`; a
  warning for `ca-created` (fresh: install it once on every device; else:
  every device must install the new certificate and remove the old one -
  Telemetry sheet, HTTPS certificate; before a supervised start it stays 5 s
  on screen before kraken_doa's dashboard takes the pane); a warning with
  the reason on failure, and the start goes on. `windows/build.sh` makes no
  certificate any more (kraken_doa's generator does, at every start); it
  only locks down `server.key` / `krakensdr-ca.key` already there (icacls).

## Tested

(filled in by the implementation)

### kraken_doa (2026-11-23, x86_64 Linux; no Windows / macOS build here)

Where it is: `tlscert::` in `src/utils/tls_compat.cpp` (generator, names,
`inspect`), `src/cert_status.cpp` (status object, notices, `CERT_SCOPE:`,
download), `verify_ssl_certificates()` in `websocket_server.cpp` (Windows:
the generator on every start; Linux / macOS: only when `server.crt` /
`server.key` are missing), the page's `cert*` functions (section in the
Telemetry sheet, `#cert-banner`). Beyond the contract's fields the status
also carries `host`, `host_ok`, `ca.sha1` (Windows shows the SHA-1 as the
Thumbprint), `server.name` / `org`, `ips_now`, `retired` (newest 3) and
`scope_error` / `scope_seq` (the outcome of a page's `CERT_SCOPE:`).
`CERT_SCOPE:` is taken from a page's own socket only (like `ANT_BIAS:`), and
refused for a certificate of the user's own.

- Generator harness (a test main linking `tls_compat.cpp`, `fs_compat.cpp`,
  `process_posix.cpp`; built against OpenSSL 3.0.2 and 1.1.1w): 89 / 89
  checks each - fresh (modes 0600 / 0644, CA and server extensions, 3650 /
  825 days, RSA 3072 / 2048, chain in `server.crt`), rerun byte-identical,
  rename -> `ca-created` + `.retired.crt` + same server key (a second CA the
  same day: `krakensdr-ca.YYYYMMDD-2.retired.crt`), scope narrow -> wide ->
  narrow via `cert_options`, a wide certificate missing an address ->
  `issued` (CA byte-identical), 59 / 61 days and expired -> `issued` /
  `kept` / `issued`, a CA with 300 days -> `ca-created`, old `CN=krakensdr`
  pairs from 3.0.2 and 1.1.1 `req -x509` and PR #5's -> migrated with the
  RSA 4096 / 2048 key kept, a user's pair and symlinked `server.crt` /
  `server.key` -> `not-managed`, byte-identical, read-only folder -> exit 1,
  files untouched. Every pair: `openssl verify -purpose sslserver` with 3.0.2,
  1.1.1w and 3.6, `curl --cacert` for every name, 127.0.0.1, ::1 and (wide)
  each LAN address. Negatives refused by all three (`permitted subtree
  violation`): router.local, evil.example and a LAN IP under a narrow CA; a
  public IP and www.example.com under a wide one (router.local under a wide
  CA is accepted - the documented risk).
- Wide scope puts only addresses inside the CA's private ranges in the
  server certificate: a public address would fail the whole certificate
  (checked: 8.8.8.8 under a wide CA is refused).
- Aligned with `make-cert.sh` rule by rule (host name and DNS-name
  validity, `KRAKEN_CERT_NAMES` split on commas / spaces without addresses,
  `cert_options` parsing, `ip -4 addr show scope global`'s addresses, a CA
  without name constraints = a scope mismatch, wide = permits 10.0.0.0/8,
  an unreadable `server.crt` = failure, an unreadable `server.key` = a new
  one, UTC dates, key modes tightened on `kept`): 42 / 42 checks, among them
  cross-runs where one makes or changes the files (fresh narrow / wide, the
  avahi name, other search domains, an unusable host name, a rename, scope
  changes, `--force`; the script with OpenSSL 3.0.2, 1.1.1w and 3.6) and
  the other then says `kept` with every file byte-identical.
- Host allow-list: `certstatus::init` adds the served certificate's DNS
  names (`host_add_allowed_names`, `utils/host_check.hpp`, identical in
  heimdall, which doesn't call it). TEST build with
  `KRAKEN_CERT_HOST=krakensdr-2`, `KRAKEN_CERT_NAMES=sdr.example.org` and
  the search domain `lan`: the download and the page's WebSocket work under
  krakensdr-2.local, krakensdr-2.lan, sdr.example.org (and the bare name);
  a name in no certificate, a foreign Host and a foreign Origin get 403 /
  a refused socket (16 / 16).
- TEST kraken_doa (ports 284xx, files made by its in-process generator):
  `/krakensdr-ca.crt` and `/krakensdr.crt` byte-identical to
  `krakensdr-ca.crt`, `application/x-x509-ca-cert`, Host / Origin check 403;
  `http://` -> 301 and then TLS verified with the CA through PR #5's socket
  hand-over; the handshake sends leaf + CA (s_client 3.0.2 and 1.1.1w:
  verify 0). Headless Chromium, real clicks (30 + 3 x 10 checks, no console
  errors): section in the Telemetry sheet, CA name and SHA-256 equal to
  `openssl x509 -subject` / `-fingerprint -sha256`, the download
  byte-identical, Narrow -> Wide (confirm) writes `cert_options` (0644) and
  shows restart_pending (section and banner), back to Narrow clears it,
  `CERT_SCOPE:` during a recording's playback refused with its reason shown
  and `cert_options` unchanged; banners for expires_soon (a 20-day server
  certificate), name_changed (`KRAKEN_CERT_HOST` changed) and
  addresses_changed (wide, one address missing), opening the section on a
  click, hidden by x until the notices change (also after a reload).
- By code only: the `_WIN32` parts (`GetAdaptersAddresses` addresses and DNS
  suffixes, reparse-point check, owner-only key files, the generator on
  every Windows start) and macOS (`scutil --get LocalHostName`) - no MinGW /
  macOS here.

### make-cert.sh (2026-11-23, x86_64 dev box, no hardware)

Scripts in the job's scratch folder (not in the repo). `harness.sh
<openssl> [port]`, run with OpenSSL 1.1.1w, 3.0.2 and 3.6.0 as the script's
openssl: **83 / 83 pass with each**. PATH shims stand in for `hostname`,
`ip`, `busctl`, `gdbus`, `avahi-resolve`, `uname`, `scutil`. Every pair
produced is checked with all three OpenSSLs (`verify -purpose sslserver`,
`-verify_hostname` per DNS name, `-verify_ip` per address), Python's
`ssl.load_cert_chain` (server.crt holds leaf + CA), the structure (one Basic
Constraints each, critical flags, key usages, SKI / AKI, CA RSA 3072,
notAfter - notBefore exactly 825 / 3650 days, notBefore an hour back,
subjects, modes 0600 / 0644), and on the fresh narrow and wide pairs with
curl `--cacert krakensdr-ca.crt` against a local Python TLS server
(127.0.0.1, every name by `--resolve`, the addresses by `--connect-to`) and
NSS `certutil -V -u V` (CA trusted `CT,,`).

| # | Case | Result |
|---|---|---|
| 1 | fresh | ca-created (`reason: first certificate`); NSS valid; curl 5 / 5 |
| 2 | rerun | kept, files byte-identical |
| 3 | old `CN=krakensdr` pair from that OpenSSL's default `req -x509`, and from 1.1.1's; build_page.py's | ca-created, server key reused |
| 3c | such an old pair put back beside our CA | issued, CA and key kept |
| 4 | PR #5's self-signed (its own script; with 1.1.1 it has 2 Basic Constraints) | ca-created, key reused |
| 5 | own certificate (`CN=sdr.example.org, O=Example`), also with `--force`; symlinked server.crt / server.key (+ `--new-ca`) | not-managed, every file byte-identical, links intact |
| 6 | IP change: narrow; wide (+ a public IP, 172.20/12, 100.64/10, link-local); a 10.x on `lo` | kept; issued, CA byte-identical, only private IPs named; left out |
| 7 | rename | ca-created, old CA `krakensdr-ca.YYYYMMDD.retired.crt`, its key gone, new names, same key |
| 8 | avahi conflict name `krakensdr-2` via busctl / gdbus / avahi-resolve; avahi-resolve's `.lan` answer | ca-created with its names; ignored |
| 9 | scope narrow -> wide -> narrow; cert_options removed; `scope=bogus`, `scope = wide`, `scope=Wide` (narrow), `  scope=wide ` (wide) | ca-created each (second retired file `-2`); kept; as kraken_doa reads it |
| 10 | leaf 61 days left; 30 days; expired; CA 400 days left; 300 days | kept; issued; issued; kept; ca-created |
| 11 | `--force`; `--new-ca` | issued (new serial, CA / key same); ca-created |
| 12 | read-only dir + `--force` / `--new-ca` / fresh; nothing to do in a read-only dir; no openssl on PATH; unknown option; garbage server.crt alone / with a key; own certificate without its key; dangling symlink; unreadable key alone / beside our certificate | exit 1, files untouched; kept; exit 1 x2; exit 1 / not-managed; exit 1 x2; exit 1 / issued with a new key |
| E | `KRAKEN_CERT_HOST` beats avahi, is trimmed; `Kraken_Roof`, `-kraken`, `kraken-`, a 64-character label, `kraken.local` (localhost only); resolv.conf `search Lan home.arpa . local` + `domain example.org.`, `search lan..`; `KRAKEN_CERT_NAMES` (names, an IP, a bad one, commas only, trailing dots; in wide under .lan / .local = issued); an EC P-256 server key; macOS via `scutil`; CA key deleted; leaf-only server.crt; server.key deleted / replaced / mode 644; a CA without name constraints (narrow kept, wide ca-created) | as above |

Negative (a leaf signed by the receiver's own CA key): a name outside the
constraints (narrow `evil.example.com`, wide `router.example.com`), a LAN IP
in narrow scope, a public IP in wide scope - rejected by OpenSSL 3.0.2 and
3.6 (`permitted subtree violation`), by NSS ("Peer's Certificate issuer is
not recognized": NSS drops the issuer whose constraints fail; a control
leaf inside the constraints, made the same way, is valid) and by curl.
**OpenSSL 1.1.1w rejects them too** (`permitted subtree violation`).

Cross-test with kraken_doa's generator (`cross.sh`; certdoa's
`tlscert::ensure` at fa2c75b built as a small driver with make-cert.sh's
interface, against OpenSSL 3.0.2 and 1.1.1w; the script with each - four
pairings; both on the real machine: `KRAKEN_CERT_HOST` set, the real
resolv.conf and interfaces): **232 / 232**. For narrow and wide: fresh by
either -> the other `kept`, then both `kept`; `--force` / `--new-ca` by
either -> the other `kept` (one retired file each); `KRAKEN_CERT_NAMES`
with spaces, a trailing dot, an IP and a bad name -> the other `kept`, and
dropped again -> `issued` -> `kept`; renames (`rooftop`, `krakensdr-2`, an
unusable `Kraken_Roof`, `" krakensdr "`) by either -> the other `kept`;
the wide IPs found the same (`ip` vs getifaddrs); a scope switch in
`cert_options` by either; an old `CN=krakensdr` pair migrated by either ->
the other `kept`; notBefore an hour back from both.

Callers (`callers.sh`: run.sh's `cert_check`, install.sh's
`install_kraken_certs`, the entrypoint's `ensure_cert`, cut out of the
files and run under each file's shell options - run.sh's `set -uo
pipefail`, install.sh's `set -Eeuo pipefail` + ERR trap, the entrypoint's
`set -euo pipefail`): the right message for kept / issued / ca-created
(fresh and replaced) / not-managed / failure with a stub script, and with
the real one (fresh, rerun); the start goes on after a failure. 24 / 24.
Supervisor (`supervise_test.sh`: a copy of run.sh in a scratch suite, only
its `__supervise` entrypoint, a fake app): cert_check before each of 3
starts - fresh (warning before the 1st), the page's Restart after a scope
change (NEW-CA warning, the 2nd start sees the wide CA), a crash restart
(nothing printed); `launch_tmux_stack` cut out with tmux stubbed: the
pane's command sets the names as run.sh had them and removes them when
unset (a stale value in the tmux server's environment). 8 / 8. Lock
(`lock.sh`): 2 / 3 runs at once on a fresh dir -> one `ca-created`, the
rest `kept`, files consistent, nothing left behind; 2 x `--new-ca` / 4 x
`--force` at once -> consistent; a stale (200 s) lock taken over at once; a
live one -> exit 1 after 10 s, files and the other's lock untouched; one
released after 3 s -> goes on; one without its time -> taken over after
10 s; a run killed by TERM while making the CA key -> exit 1, folder
empty again; a read-only folder -> kept. 20 / 20. `.githooks/pre-push`
(`test_prepush.sh`, a scratch repo): a pushed `krakensdr-ca.key` /
`server.key` (any folder) is blocked, `krakensdr-ca.crt` and
`docs/myserver.key` are not. `.gitignore` ignores every certificate file,
the script's temporary folder and its lock (`git check-ignore`).

Shells: the whole matrix also passes with the script run by `dash` (it
uses POSIX sh plus `local` only - no arrays, `[[`, `${x,,}`, `$'...'`) and
by `bash --posix` with `BASH_COMPAT=3.2` (83 / 83 each).

Not tested: bash 3.2 itself (macOS; not available here - the dash run is
the stand-in), macOS's `scutil` / `ifconfig -u` and LibreSSL (only through
a shim / not at all), the Docker image (no Docker here: the entrypoint
block was run alone), a real avahi conflict (shims), headless mode's
cert_check and the build window's env (by code: the same function / the same
`env` pattern), `windows/build.sh` (no MinGW / MSYS2 here), browsers' trust
of the CA (Chrome for Testing here ignores a user NSS store; NSS and curl
stand in).
