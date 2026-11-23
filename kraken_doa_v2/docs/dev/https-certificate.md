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
| risk if the CA key leaks | it can vouch only for this receiver's names | it can impersonate any `.local` / `.lan` name and any private IP (router, NAS) to devices that installed it |

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
