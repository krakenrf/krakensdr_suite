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
  `.crt` from one to the others (before their first start, or later: the
  next start re-issues the server certificate from the copied CA - Fine
  rule 22); one install then covers them all, as long as each receiver's
  names are inside its constraints (wide scope, or `KRAKEN_CERT_NAMES` on
  the first, made with `--new-ca`).

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
22. **Managed also after the CA was replaced or deleted**: besides rule 15
    and "verifies against `krakensdr-ca.crt`", a `server.crt` is ours when it
    verifies (no time check) against one of the folder's
    `krakensdr-ca.*.retired.crt`, or when BOTH its subject and its issuer
    have `O=KrakenSDR receiver` and its issuer's CN starts `KrakenSDR ` and
    contains ` CA ` (a CA of ours that was deleted, or replaced by a copied
    one - sharing a CA, "The CA"). Such a pair is re-issued from the current
    CA (`issued`, or `ca-created` when there is none), keeping `server.key`.

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

How the two makers - `scripts/make-cert.sh` and kraken_doa's generator
(`tlscert::ensure`, `src/utils/tls_compat.cpp`) - settle what the contract
and its Fine rules leave open. Each finds the other's files `kept`
(cross-tested, see *Tested*); change both together.

- **Host name** (rules 6, 7). Linux: avahi over D-Bus first (`busctl call
  org.freedesktop.Avahi / org.freedesktop.Avahi.Server GetHostName`, else the
  same call through `gdbus`), after waiting up to 3 s (6 polls, 0.5 s
  apart) while `GetState` says registering (1) / collision (3) - asked
  through busctl, or gdbus when there is no busctl; then `avahi-resolve -a
  <the first private IPv4 address>` - only an answer ending in `.local`
  counts (on the dev box it answered with the router's `<host>.lan` name);
  then `hostname -s` / `hostname` / `uname -n` (the generator:
  `gethostname()`), cut at the first dot and trimmed. macOS: `scutil --get
  LocalHostName`. `KRAKEN_CERT_HOST` is trimmed (space, tab, CR, LF, VT,
  FF) and lowercased, not cut: a dot in it makes it unusable. Lowercasing is
  ASCII only in both (the script runs under `LC_ALL=C`; lengths are bytes).
- **DNS names** = labels of 1-63 `a-z 0-9 -` (not starting / ending with
  `-`) joined by dots, 253 characters at most. resolv.conf: every word after
  `search` / `domain`, lowercased, all trailing dots removed; `local`, and
  what is no DNS name then (systemd-resolved's `search .`), are skipped, as
  is a domain that makes `<host>.<d>` longer than 253.
- **`KRAKEN_CERT_NAMES`** (rule 4): split on commas and white space,
  lowercased, trailing dots removed. Left out, each with a note on stderr:
  an IPv4 address in `inet_pton`'s form (four decimal parts 0-255, no
  leading zero - written out in both makers, so every platform decides
  alike) or an IPv6 address (it has a `:`), and any other word that is no
  DNS name. `010.1.2.3` and `1.2.3` are no `inet_pton` address: they stay
  DNS names (valid labels).
- **Wide scope's addresses** (rule 8): the script `ip -4 -o addr show up`
  without the `lo` interface (macOS: `ifconfig -u` without loopback
  interfaces; else `hostname -I`); the generator `getifaddrs()` with
  `IFF_UP`, without `IFF_LOOPBACK` and without `lo` (an alias address's
  label `lo:1` counts as `lo`); Windows `GetAdaptersAddresses` (up, not the
  loopback adapter). Then without 127/8 and 169.254/16, and only those in
  10/8, 172.16/12, 192.168/16, 100.64/10 (a public address would fail the
  whole certificate: 8.8.8.8 under a wide CA is refused). `127.0.0.1`,
  `::1` always. `addresses_changed` uses the same filter.
- **`cert_options`** (rule 5): a line `scope = VALUE` - the key in any
  case, spaces / tabs around `=`, the value trimmed (a CR of a file edited
  on Windows too) and lowercased; the last such line wins (`scopes=...`
  and `scope wide` are no such lines). Not `wide` or `narrow`: a note,
  narrow.
- **The server certificate's names** (rule 18): the wanted DNS names the
  CA permits. A wanted name outside the CA's DNS subtrees that is one of the
  host's OWN names (`<host>`, `<host>.local` - a rename, an avahi conflict
  name) makes a new CA; any other (a search domain of another network, a
  `KRAKEN_CERT_NAMES` name added later; `localhost` too, which our CAs
  always permit) is left out and reported: the script prints `not-covered:
  <names>` (and a note on stderr; run.sh's `cert_check` shows a warning with
  the `--new-ca` hint), the generator a note on stderr, the status
  `names_not_covered` (below). A new CA (any trigger) takes every name in.
  Moving between two networks with other search domains re-issues the
  server certificate (`issued`), never the CA.
- **"Names / IPs differ"**: the SAN set compared sorted, order-free, DNS
  names case-insensitively (`::1` is printed as `0:0:0:0:0:0:0:1`); an entry
  of another kind (e-mail, URI, an IP of another length) never matches.
- **The CA** - a new one (`ca-created`) when (in this order for the reason
  printed): `--new-ca`; `krakensdr-ca.key` unusable (moved aside, rule 12);
  no CA certificate / key, or they don't match, or no `CA:TRUE`; fewer than
  365 days left (`-checkend` / `X509_cmp_time`, the same test); scope `none`
  - no Name Constraints (rule 3); partial constraints - no permitted DNS
  subtree, or no permitted IP subtree (each half leaves those names
  unconstrained: rules 3, 18; the IP half is this implementation's reading
  of "partial"); `extendedKeyUsage` not exactly serverAuth (rule 16; the
  script: the line `TLS Web Server Authentication`, the generator: one OID,
  `NID_server_auth`); another scope (wide = a permitted IP subtree
  `10.0.0.0/255.0.0.0`); an own name outside it. A wanted DNS name is inside
  a permitted `DNS:c` when it is `c` or ends in `.c` (`.c`: only under it).
  The CA certificate's first certificate counts in both makers (the file may
  hold more - a key too: it is never served, rule 19).
- **Managed** (rules 10, 15): issued by the first certificate of
  `krakensdr-ca.crt` - `openssl verify -no-CApath -no-CAstore
  -no_check_time -purpose sslserver -CAfile <it>` / `X509_verify_cert` with
  `X509_V_FLAG_NO_CHECK_TIME`: chain, signature, constraints, purpose, no
  time - is ours (the script reuses that one verify for the re-issue check:
  the `kept` path costs six openssl calls, as before). Else only a legacy
  subject (`O=KrakenSDR receiver`, or exactly `CN=krakensdr`) that is
  self-signed: issuer = subject and its signature verifies with its own
  key. The script checks the signature by itself (`asn1parse` for the TBS
  and signature offsets, `dd`, `dgst -<md> -verify`; `openssl verify` would
  also judge the extensions - build_page.py's certificate has no
  keyCertSign), for RSA PKCS#1 and ECDSA with SHA-1 / SHA-2 - what every old
  installer made; the generator takes `X509_verify` for exactly those
  signature algorithms, so an RSA-PSS / Ed25519 legacy certificate is a
  user's own in both. Before a legacy (migrated) `server.crt` is replaced,
  it is copied to `server.crt.replaced-YYYYMMDD` (0644; the script prints
  `saved: <file>`). Anything else - also a `CN=krakensdr` certificate from a
  home CA, or a self-issued one signed by another key - is `not-managed`
  with every file untouched (it needs `server.key`; without it exit 1).
- **Unreadable files** (rule 12): after the symlink check, a `server.crt`,
  `server.key`, `krakensdr-ca.crt` or `krakensdr-ca.key` this user may not
  read (script `test -r`, generator `fopen` failing with EACCES / EPERM) is a
  failure naming the file ("check its owner"), every file untouched - also
  `--new-ca`. A key that can be read but not used (encrypted - the script
  passes an empty `-passin`, the generator a callback giving no passphrase -
  or garbage) is renamed to `<name>.unreadable-YYYYMMDD` (`-2` ...; made
  0600) right before its replacement goes into place; the script prints
  `moved: <file>`. With a user's own `server.crt` nothing is touched
  (`not-managed`). An EMPTY `server.crt` / `server.key` counts as missing in
  both (no certificate / key in it to lose: the generator also runs when one
  is empty) - this implementation's reading of rules 11 / 12.
- **Re-issue** (rules 9, 10): also when `server.crt` isn't exactly [leaf,
  current CA] (a leaf-only file), when it doesn't match `server.key` (the key
  is kept), when `server.key` is missing / unusable. Expiry by `-checkend`,
  read from its MESSAGE (OpenSSL 3.0.2 exits 0 for "Certificate will expire"
  too - PR #5's renewal check never fired there); the generator with
  `X509_cmp_time(notAfter, now + 60 d) < 0`, exactly `-checkend`'s test. A
  certificate not valid yet (a clock behind it) is kept (no time check).
- **Validity** (rule 9): notBefore = now - 1 hour, notAfter = notBefore +
  825 / 3650 days exactly. The script signs with `openssl ca -startdate
  -enddate` (`-selfsign` for the CA, a throw-away database in the stage
  folder: the one way 1.1.1, 3.x and LibreSSL can all set the start), the
  generator takes one start time for both certificates (`ASN1_TIME_set`;
  UTCTime before 2050 in both). Serial numbers: 128 random bits (the
  generator 127, positive).
- **The CA certificate** gets `extendedKeyUsage = serverAuth` (rule 16) and
  `CN=KrakenSDR <host, its first 40 characters> CA <UTC date>` (rule 17;
  the server certificate's CN is the whole host name, 63 at most).
- **Dated names** (rules 1, 2, 12, 15): `krakensdr-ca.YYYYMMDD.retired.crt`,
  `<key>.unreadable-YYYYMMDD`, `server.crt.replaced-YYYYMMDD`, then `-2`,
  `-3` ... by which names are taken - a dangling symlink takes one too
  (`-e || -L` / `lstat`); the date `date -u` / `gmtime`. Only a readable,
  non-symlink CA certificate is retired; the new CA key replaces the old one
  by rename (its key is gone).
- **Modes** (rule 13): both keys 0600 at the end of every `kept` / `issued`
  / `ca-created` run (a symlinked key - not ours - is left alone, its
  target too); Windows: the generator gives an existing key the owner-only
  DACL again (this user, SYSTEM, Administrators; untested - no MinGW here).
- **Lock** (rule 14): `<dir>/.make-cert.lock` (mkdir / `create_directory`;
  it holds `since`, Unix seconds), waited for up to 10 s, then exit 1 /
  an error ("another make-cert.sh or kraken_doa is writing the
  certificate"). A dead one - older than 120 s, dated more than 60 s ahead,
  or without its time after the wait - is renamed away to
  `.make-cert.stale.<pid>` (the generator `.make-cert.stale.<random>`):
  only one of two waiters' renames can succeed; the winner reads the renamed
  lock's time again - not the one judged dead (a live lock made meanwhile)
  -> renamed back. A read-only `<dir>` gets no lock (a dead lock that can't
  be renamed there neither). Removed on exit, also after TERM / INT (the
  script's trap; the generator's guard object).
- **Output** before the last line: `reason: <text>` (issued / ca-created /
  not-managed), `retired: <file>`, `moved: <file>`, `saved: <file>`,
  `not-covered: <names>` (also on `kept`); a fresh install says exactly
  `reason: first certificate` (callers word their message by it). Notes (bad
  `cert_options`, a name left out, an unusable host name, a key moved aside)
  go to stderr - the generator's as `HTTPS certificate: ...` lines.
- **Commit order** (each a rename in `<dir>`): a migrated `server.crt`'s
  copy, old CA certificate -> retired, unusable CA key -> aside, new CA key,
  new CA certificate, unusable server key -> aside, new server key,
  `server.crt`. An interruption leaves a state the next run repairs (a CA
  key without its certificate -> new CA; a key without its certificate ->
  re-issue).
- **kraken_doa's side**: `/krakensdr-ca.crt` is the parsed first
  certificate re-encoded (`PEM_write_bio_X509`; for a plain file the same
  bytes) - never the file (rule 19). `names_not_covered` in the status (and
  the notice of that name, the page's banner and section): the wanted names
  that are not own and outside the served CA's DNS subtrees - empty when the
  next start replaces the CA anyway (an own name outside, another scope, no
  DNS subtree). The host allow-list (rule 20) takes from the served
  certificate's names only `<host>`, `<host>.local`, `KRAKEN_CERT_NAMES`'
  and `<host>.lan` / `.home` / `.localdomain` / `.home.arpa`.
  `tlscert::inspect` lists the retired CAs without exceptions
  (`increment(error_code)`, `u8string()`, a catch around it: a name MinGW
  can't convert must not crash `certstatus::init` at every start).
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
  on screen before kraken_doa's dashboard takes the pane); run.sh also warns
  for `not-covered:` names and for files kept aside (`moved:` / `saved:`); a
  warning with the reason on failure, and the start goes on.
  `windows/build.sh` makes no certificate any more (kraken_doa's generator
  does, at every start); it only locks down `server.key` /
  `krakensdr-ca.key` already there (icacls).

## Tested

### Both makers, Fine rules 1-21 (2026-11-23, x86_64 dev box, no hardware)

Scripts in the job's scratch folder (`w_certalign/`, not in the repo); the
generator is `tlscert::ensure` of this tree built as a small driver with
make-cert.sh's interface (`doa/certgen.cpp` + `tls_compat.cpp`,
`fs_compat.cpp`, `process_posix.cpp`), against OpenSSL 3.0.2 and 1.1.1w.

- **Cross-test** (`script/cross.sh`): **824 / 824**, four pairings (the
  script with 3.0.2 / 1.1.1w x the generator against 3.0.2 / 1.1.1w), both
  on the real machine (the real resolv.conf and interfaces - `ip` vs
  `getifaddrs`; avahi shimmed for both). For narrow and wide, with each maker
  making or changing the files in turn - fresh, `--force`, `--new-ca`,
  `KRAKEN_CERT_NAMES` added later (left out; wide: `cam.lan` taken),
  `--new-ca` taking them in, dropped again, another network's search domain
  and back (`issued`, no new CA), renames (`rooftop`, a 50-character name,
  back), an unusable `Kraken_Roof`, `kraken.local`, `" Krakensdr<tab>"`, an
  unusable server key (moved aside), keys left 0644, the scope switched by
  `SCOPE = Wide` / `scope=wide` + `scope = NARROW`, the retired names `-2`
  ... `-7`, an unusable CA key (moved aside), an old `CN=krakensdr` pair and
  PR #5's (1.1.1, two Basic Constraints) migrated with `server.crt.replaced-`,
  a leaf-only `server.crt`, `server.crt` deleted, the CA key deleted, a CA
  without name constraints, without extendedKeyUsage, with IP subtrees only,
  a wide-looking CA without 10/8, the system host name, avahi via busctl /
  gdbus / avahi-resolve - the other then says `kept` with every file
  byte-identical (content, mode, the names in the folder). States made by
  hand that both decide alike: 12 `cert_options` spellings (CRLF too), 4
  `KRAKEN_CERT_NAMES` spellings of one set, 6 unusable host spellings, the
  dates of either's files (notBefore now - 1 h, exactly 825 / 3650 days, CN
  date UTC), a leaf valid only from in 2 days (kept), garbage / a broken PEM
  block in `server.crt` (exit 1 both), a user's own certificate beside an
  unusable key, `CN=krakensdr` from a home CA, a self-issued `O=KrakenSDR
  receiver` signed by another key (not-managed both, nothing touched), a CA
  key this user may not read (exit 1 both).
- **The script** (`script/harness.sh <openssl> <port>`): **122 / 122** with
  OpenSSL 1.1.1w, 3.0.2 and 3.6.0 as its openssl, under `dash`, and under
  `bash --posix` with `BASH_COMPAT=3.2`. PATH shims stand in for
  `hostname`, `ip`, `busctl`, `gdbus`, `avahi-resolve`, `uname`, `scutil`.
  Every pair produced is checked with all three OpenSSLs (`verify -purpose
  sslserver`, `-verify_hostname` per DNS name, `-verify_ip` per address),
  Python's `ssl.load_cert_chain`, the structure (one Basic Constraints each,
  critical flags, key usages, the CA's extendedKeyUsage = serverAuth only, its
  CN at most 64, SKI / AKI, CA RSA 3072, notAfter - notBefore exactly 825 /
  3650 days, notBefore an hour back, subjects, modes 0600 / 0644), curl
  `--cacert` against a local TLS server for every name and address, NSS
  `certutil -V -u V`. Cases beyond the first round's (fresh, rerun,
  migrations, own certificates, symlinks, IP changes, renames, avahi, scope,
  expiry, `--force` / `--new-ca`, read-only folders, no openssl, the name
  rules): rule 1 under `TZ=Pacific/Kiritimati` and `Etc/GMT+12`; rule 3 /
  16 / 18 CAs re-made by hand (control kept; no EKU, serverAuth + clientAuth,
  IP-only, DNS-only, no constraints -> new CA); rule 4 / 5 spellings; rule 7
  GetState polled 3 times / 6 times (~3 s) through busctl and through gdbus
  without busctl, avahi-resolve asked for the private address after a public
  one; rule 10 a leaf not valid yet; rule 11 garbage / a broken PEM block
  (exit 1, untouched); rule 12 an unusable server key (no `server.crt`,
  beside ours, a second the same day `-2`), each of the four files at mode
  000 (exit 1 "check its owner", untouched, also `--new-ca`), an encrypted
  CA key (moved aside, new CA), an own certificate + an unusable key
  (untouched), a read-only folder; rule 13 keys 0644 / 0640 on kept and
  issued, a symlinked CA key's target left alone; rule 15 `saved:` copies
  (`-2` the same day), `CN=krakensdr` from a home CA (also `--force`) and
  self-issued certificates signed by another key (not-managed, untouched),
  build_page.py's (no keyCertSign) and an EC P-256 pair migrated; rule 17 41-
  and 63-character host names; rule 18 names added later (kept +
  `not-covered:`), `--new-ca`, roaming between `search fritz.box` and `lan`
  (`ca-created issued issued issued`, no retired CA). Negatives (a leaf
  signed by the receiver's own CA key for a name / address outside its
  constraints) refused by OpenSSL 1.1.1w, 3.0.2, 3.6, NSS and curl.
- **The generator** (`doa/gen_tests.sh`, `doa/align_tests.sh`,
  `doa/nss_check.sh`): 92 / 92 against 3.0.2 and against 1.1.1w (fresh with
  every extension incl. the CA's serverAuth, rerun byte-identical, rename,
  scope, IP change, expiry 59 / 61 days / expired, a 300-day CA, old pairs
  migrated with `server.crt.replaced-`, own pairs and symlinks, read-only,
  negatives refused by three OpenSSLs); 42 / 42 alignment checks with the
  script; NSS: every pair valid, the negatives "issuer is not recognized".
- **Lock** (`script/lock.sh`): 60 / 60 - for the script and the generator
  each: a 200-s-old lock and one dated 30 min ahead taken over at once, one
  30 s ahead live (exit 1 after 10 s, kept), a live one (exit 1 after 10 s,
  files and lock untouched, the message), released after 3 s (goes on), one
  without its time (taken over after 10 s), a read-only folder with a dead
  lock (kept); two waiters on one dead lock (script + script, script +
  generator, generator + generator, 3 rounds each): one `ca-created`, one
  `kept`, consistent, nothing left; 2 / 3 runs at once fresh in 6
  script / generator mixes; `--new-ca` by both at once (2 retired CAs); 4 x
  `--force` mixed; the generator's lock holds `since` in the script's format
  and is gone afterwards; the script killed by TERM mid-way (folder empty,
  the generator's next run `ca-created`).
- **kraken_doa's status** (`review/certstatus_test.sh`, the review's driver
  linking the real `cert_status.cpp` + `tls_compat.cpp`): 20 / 20 - the
  download of a CA file holding cert + key or key + cert is the certificate
  alone (re-encoded, equal to `openssl x509` of it), a plain file's
  byte-identical, an own `server.crt` with a key in it gives its certificate
  only; `names_not_covered` empty, then `krakensdr.fritz.box
  sdr.example.org` for another network + `KRAKEN_CERT_NAMES` (notice too),
  empty for a rename (`name_changed` instead), and after the script left a
  name out; the allow-list for a certificate made with `search lan
  attacker.example`: krakensdr-2, .local, .lan, `KRAKEN_CERT_NAMES`' name,
  localhost allowed, `krakensdr-2.attacker.example` (in the certificate) and
  evil.example refused.
- The review's own tests (`review/t1-t8`) rerun against this tree: the CA
  download holds no key; an encrypted key is moved aside (script and
  generator), keys this user can't read fail with "check its owner",
  nothing changed; a lock dated 30 min ahead is taken over at once; a
  `CN=krakensdr` certificate from a home CA is not-managed and untouched (both);
  roaming home / car / home / car makes one CA; 40-, 41-, 63-character host
  names all make a pair; rules 3 / 4 / 5 / 6 / 11 as above.
- AI assistant (`ai_secrets_test.py`, `claude_settings()` loaded with
  `/usr/bin/python3 -I`): 17 / 17 - Read + Edit denied for server.key,
  krakensdr-ca.key and the patterns `*.unreadable-*`, `*.key.tmp`,
  `.make-cert.*` (each also with `/**`), the sandbox's denyRead names them
  as patterns, with `/**`, and by the names present (a moved-aside key, a
  make-cert folder made for the test).
- `.githooks/pre-push` (`script/test_prepush.sh`, a scratch repo): 13 / 13 -
  a pushed `krakensdr-ca.key` / `server.key` (any folder),
  `*.unreadable-*`, `server.key.tmp`, a file in `.make-cert.lock/` or in a
  top-level `.make-cert.*/` blocked; `krakensdr-ca.crt`,
  `server.crt.replaced-*`, `docs/myserver.key` not. `.gitignore`
  (`git check-ignore --no-index`): 16 / 16 of these names ignored in
  `kraken_doa_v2/`, `docker-data/` and any other folder.
- Callers (`script/callers.sh`): 26 / 26 (the first round's 24, plus run.sh's
  `not-covered:` warning and the `moved:` / `saved:` line); supervisor
  (`script/supervise_test.sh`): 8 / 8. The page's `cert*` block in node with
  stubs (`page_cert_test.js`): 9 / 9 (the `names_not_covered` text, escaped
  names, the banner hidden until the names change, an older status without
  the array); both inline scripts parse (`node --check`, node 24).
- `kept` costs 51 ms and six openssl calls (`script/timing.sh`). Full builds
  of kraken_doa and heimdall (`host_check.hpp`'s comment) without new
  warnings.
- By code only: the `_WIN32` parts (the owner-only DACL on existing keys,
  `GetAdaptersAddresses`, the lock through `std::filesystem` and `fopen`'s
  EACCES on MinGW, `u8string()` in `inspect`) - no MinGW / zig here;
  macOS (`scutil`, `ifconfig -u`, `getifaddrs`' flags, LibreSSL's
  `asn1parse` / `dgst` output); bash 3.2 itself (dash and `bash --posix`
  stand in); the Docker image; a real avahi conflict (shims); an interface
  that is down with an address (none here: the `IFF_UP` filter is by code);
  thread exhaustion in the pre-open hook (the catch is by code); browsers'
  trust of the CA (Chrome for Testing here ignores a user NSS store; NSS and
  curl stand in); the page's new notice in a browser (its code ran in node).

### kraken_doa, first round (2026-11-23, certdoa; not rerun)

Where it is: `tlscert::` in `src/utils/tls_compat.cpp` (generator, names,
`inspect`), `src/cert_status.cpp` (status object, notices, `CERT_SCOPE:`,
download, the allow-list), `verify_ssl_certificates()` in
`websocket_server.cpp` (Windows: the generator on every start; Linux /
macOS: only when `server.crt` / `server.key` are missing), the page's
`cert*` functions (section in the Telemetry sheet, `#cert-banner`). Beyond
the contract's fields the status also carries `host`, `host_ok`, `ca.sha1`
(Windows shows the SHA-1 as the Thumbprint), `server.name` / `org`,
`ips_now`, `names_not_covered`, `retired` (newest 3) and `scope_error` /
`scope_seq` (the outcome of a page's `CERT_SCOPE:`). `CERT_SCOPE:` is taken
from a page's own socket only (like `ANT_BIAS:`), and refused for a
certificate of the user's own.

- TEST kraken_doa (ports 284xx, files made by its in-process generator):
  `/krakensdr-ca.crt` and `/krakensdr.crt` byte-identical to
  `krakensdr-ca.crt` (a plain file - still so after rule 19's re-encoding),
  `application/x-x509-ca-cert`, Host / Origin check 403; `http://` -> 301
  and then TLS verified with the CA through PR #5's socket hand-over; the
  handshake sends leaf + CA (s_client 3.0.2 and 1.1.1w: verify 0). Host
  allow-list (before rule 20 narrowed it to the host's own names,
  `KRAKEN_CERT_NAMES`' and the trusted suffixes): krakensdr-2.local,
  krakensdr-2.lan, sdr.example.org and the bare name worked; a name in no
  certificate, a foreign Host and a foreign Origin got 403 / a refused socket
  (16 / 16). Headless Chromium, real clicks (30 + 3 x 10 checks, no console
  errors): section in the Telemetry sheet, CA name and SHA-256 equal to
  `openssl x509 -subject` / `-fingerprint -sha256`, the download
  byte-identical, Narrow -> Wide (confirm) writes `cert_options` (0644) and
  shows restart_pending (section and banner), back to Narrow clears it,
  `CERT_SCOPE:` during a recording's playback refused with its reason shown
  and `cert_options` unchanged; banners for expires_soon (a 20-day server
  certificate), name_changed (`KRAKEN_CERT_HOST` changed) and
  addresses_changed (wide, one address missing), opening the section on a
  click, hidden by x until the notices change (also after a reload).
