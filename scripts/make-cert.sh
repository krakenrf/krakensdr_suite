#!/usr/bin/env bash
#
# scripts/make-cert.sh - the HTTPS certificate of kraken_doa's web UI (8080)
#
#   scripts/make-cert.sh <dir> [--force] [--new-ca]
#
# Implements kraken_doa_v2/docs/dev/https-certificate.md, the contract this
# script shares with kraken_doa's in-process generator
# (kraken_doa_v2/src/utils/tls_compat.cpp, tlscert::ensure) and the page;
# its "Fine rules" settle every detail both follow: change them together -
# each must find the other's files `kept`. In short,
# each receiver has its own small certificate authority (krakensdr-ca.key /
# krakensdr-ca.crt), which users install once per device. It signs the
# server certificate (server.crt = the server certificate followed by the CA
# certificate), which is re-issued here whenever the names or addresses
# change or it gets old - always for the same server.key - without anyone
# having to install anything again. The CA itself is replaced only when it
# can't vouch for the host's own name (a rename, an avahi conflict name),
# when the scope in cert_options differs from its own, when it expires
# within a year, when its constraints are missing or partial, when it isn't
# limited to TLS servers, or with --new-ca; another name outside it (a
# search domain of another network) is left out of the server certificate
# and reported. The old CA certificate is kept as
# krakensdr-ca.YYYYMMDD.retired.crt (its key deleted) so the page can name
# what devices should remove. A server certificate of an earlier CA of ours
# (one retired, deleted, or replaced by another receiver's CA copied in -
# sharing a CA) is re-issued from the current one. A certificate of the
# user's own (also one with CN=krakensdr from another CA, or a symlinked
# server.crt / server.key) is never touched; a key that can't be used is
# moved aside, never overwritten.
#
#   <dir>      kraken_doa's working directory (kraken_doa_v2, /data/kraken_doa)
#   --force    re-issue the server certificate (same key, same CA)
#   --new-ca   make a new CA (every device then installs it again)
#
# Environment: KRAKEN_CERT_HOST (the host name, instead of the one avahi
# announces / hostname -s / scutil), KRAKEN_CERT_NAMES (more DNS names,
# separated by commas or spaces), OPENSSL (the openssl command to use); for
# tests only: KRAKEN_CERT_RESOLV_CONF (read instead of /etc/resolv.conf).
#
# Output (stdout): "reason: <why>" for issued / ca-created / not-managed,
# "retired: <file>" when a CA was replaced, "moved: <file>" for an unusable
# key moved aside, "saved: <file>" for a migrated server.crt's copy,
# "not-covered: <names>" for wanted names outside the CA (left out of the
# server certificate; --new-ca takes them in), then as the LAST line:
#   kept         nothing changed
#   issued       the server certificate was (re-)issued, the CA is unchanged:
#                devices that installed the CA need nothing
#   ca-created   a new CA: every device must install it (and remove the
#                retired one); "reason: first certificate" on a fresh install
#   not-managed  a certificate of the user's own: left alone
# Exit 0 in all four cases. Failure: exit 1, the reason on stderr, every
# file as it was (everything is made in a temporary folder in <dir> first
# and moved into place at the end; keys 0600 - also on `kept` -, certificates
# 0644). Two runs at once take turns (<dir>/.make-cert.lock, waited for up
# to 10 s; one older than 120 s is a dead run's and taken over), also with
# kraken_doa's generator, which takes the same lock. The `kept` path makes
# no key and costs six openssl calls.
#
# Portable: bash 3.2 (macOS: no arrays, no [[, no ${x,,}, no mapfile - it
# runs under dash as well), OpenSSL 1.1.1 / 3.x / LibreSSL - only its own
# config files (-config, -extfile), never -addext on top of the system's
# openssl.cnf (1.1.1 then added a second Basic Constraints extension), and
# names / constraints read back with x509 -text (1.1.1 and LibreSSL have no
# x509 -ext). Certificates are signed with `openssl ca`, the one way all of
# them can set the start date (an hour back, like kraken_doa's generator: a
# device clock a little behind still accepts a certificate made just now).

set -u
set -f          # no pathname expansion: the lists below are split on spaces
umask 077       # keys and the work folders; certificates are chmod 644
# bytes and ASCII: tr / sed / ${#x} as kraken_doa's generator counts and
# lowercases, whatever the user's locale
LC_ALL=C; export LC_ALL

me=make-cert.sh
usage="usage: $me <dir> [--force] [--new-ca]"
fail() { echo "$me: $*" >&2; exit 1; }
note() { echo "$me: $*" >&2; }

dir=""; force=0; new_ca=0
for a in "$@"; do
    case "$a" in
    --force)   force=1 ;;
    --new-ca)  new_ca=1 ;;
    -h|--help) awk 'NR > 2 && !/^#/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
    -*)        fail "unknown option $a ($usage)" ;;
    *)         [ -z "$dir" ] || fail "one directory only ($usage)"; dir=$a ;;
    esac
done
[ -n "$dir" ] || fail "$usage"
[ -d "$dir" ] || fail "$dir is not a directory"
D=$(cd "$dir" 2>/dev/null && pwd) || fail "cannot enter $dir"
[ "$D" != / ] || D=""
CA_KEY="$D/krakensdr-ca.key"; CA_CRT="$D/krakensdr-ca.crt"
KEY="$D/server.key"; CRT="$D/server.crt"; OPTS="$D/cert_options"
LOCK="$D/.make-cert.lock"
RESOLV=${KRAKEN_CERT_RESOLV_CONF:-/etc/resolv.conf}
OS=$(uname -s 2>/dev/null) || OS=unknown

# --- openssl ------------------------------------------------------------------
OSSL=${OPENSSL:-}
if [ -z "$OSSL" ] && [ "$OS" = Darwin ]; then
    # macOS's own openssl is LibreSSL; install.sh installs Homebrew's openssl@3
    for c in /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl; do
        if [ -x "$c" ]; then OSSL=$c; break; fi
    done
fi
[ -n "$OSSL" ] || OSSL=$(command -v openssl 2>/dev/null) || OSSL=""
VER=""
[ -z "$OSSL" ] || VER=$("$OSSL" version 2>/dev/null </dev/null) || VER=""
[ -n "$VER" ] || fail "no openssl command (it makes the web UI's certificate) - nothing was changed"
ossl() { "$OSSL" "$@" </dev/null; }

# verify options: only our CA counts, not the system's trust store as well;
# -no_check_time for the managed check (an expired certificate of ours is
# still ours). By the version where it is known, else by verify -help
case "$VER" in
"OpenSSL 1.1."*) V_ONLY="-no-CApath"; V_NOTIME="-no_check_time" ;;
"OpenSSL "[3-9]*) V_ONLY="-no-CApath -no-CAstore"; V_NOTIME="-no_check_time" ;;
*)  vhelp=$(ossl verify -help 2>&1); V_ONLY=""; V_NOTIME=""
    case "$vhelp" in *-no-CApath*) V_ONLY="-no-CApath" ;; esac
    case "$vhelp" in *-no-CAstore*) V_ONLY="$V_ONLY -no-CAstore" ;; esac
    case "$vhelp" in *-no_check_time*) V_NOTIME="-no_check_time" ;; esac ;;
esac
# the CA's SHA-256 (fine rule 26): x509 -fingerprint -sha256 in OpenSSL; LibreSSL's
# x509 documents -md5 / -sha1 only - there the DER through dgst -sha256
case "$VER" in "OpenSSL "*) FP_OPT="-fingerprint -sha256" ;; *) FP_OPT="" ;; esac

WORK=$(mktemp -d "${TMPDIR:-/tmp}/make-cert.XXXXXX" 2>/dev/null) || fail "cannot make a temporary folder"
STAGE=""; LOCKED=0
cleanup() {
    rm -rf "$WORK"
    if [ -n "$STAGE" ]; then rm -rf "$STAGE"; fi
    if [ $LOCKED = 1 ]; then rm -rf "$LOCK"; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
# without -no-CApath (LibreSSL): an empty -CApath keeps the system's trust
# store out of "issued by our CA"
if [ -z "$V_ONLY" ] && mkdir "$WORK/noca" 2>/dev/null; then V_ONLY="-CApath $WORK/noca"; fi
NOW=$(date -u +%s)
case "$NOW" in ""|*[!0-9]*) fail "date +%s gave no time - nothing was changed" ;; esac

# must <what> <openssl arguments...>: run openssl in the stage folder (`ca`
# keeps its database there), or fail with its last error
must() {
    local what=$1; shift
    ( cd "$STAGE" && "$OSSL" "$@" ) </dev/null >"$WORK/out" 2>"$WORK/err" && return 0
    fail "openssl could not $what: $(grep -v '^[[:space:]]*$' "$WORK/err" | tail -n 1) - nothing was changed"
}
nap() { sleep 0.2 2>/dev/null || sleep 1; }

# --- one run at a time in <dir> -----------------------------------------------
# Fine rule 14. mkdir is atomic everywhere; the folder holds the time it was
# taken (Unix seconds in "since"). kraken_doa's generator takes the same lock
# the same way (tlscert's DirLock). A dead run's lock - older than 120 s,
# dated more than 60 s ahead (the clock was set back: a Pi without a clock
# after a power cut), or without its time once the wait is over - is taken
# over by renaming it away first: of two waiters only one rename succeeds,
# and one that renamed a NEW lock (made meanwhile) puts it back. A folder
# that can't be made at all (read-only <dir>): no lock - nothing can be
# written there anyway
lock_since() { local v; v=$(cat "$1/since" 2>/dev/null) || v=""; case "$v" in *[!0-9]*) v="" ;; esac; printf '%s' "$v"; }
take_lock() {
    local t0 now since st
    t0=$(date +%s)
    st="$D/.make-cert.stale.$$"
    while :; do
        if mkdir "$LOCK" 2>/dev/null; then
            LOCKED=1
            date +%s > "$LOCK/since" 2>/dev/null
            return 0
        fi
        [ -d "$LOCK" ] || return 0
        now=$(date +%s)
        since=$(lock_since "$LOCK")
        if { [ -n "$since" ] && { [ $((now - since)) -gt 120 ] || [ $((since - now)) -gt 60 ]; }; } ||
           { [ -z "$since" ] && [ $((now - t0)) -ge 10 ]; }; then
            rm -rf "$st" 2>/dev/null
            if mv "$LOCK" "$st" 2>/dev/null </dev/null; then
                # the one judged dead? else a live one made meanwhile: back
                if [ "$(lock_since "$st")" = "$since" ]; then rm -rf "$st"
                else mv "$st" "$LOCK" 2>/dev/null </dev/null || rm -rf "$st"; fi
                continue
            fi
            # can't be renamed while it is still the dead one: a read-only
            # <dir> - nobody can write there
            if [ -d "$LOCK" ] && [ "$(lock_since "$LOCK")" = "$since" ]; then return 0; fi
            continue
        fi
        [ $((now - t0)) -lt 10 ] ||
            fail "another make-cert.sh or kraken_doa is writing the certificate in ${D:-/} (for 10 s) - nothing was changed"
        nap
    done
}
take_lock
# fine rule 34: a dead run's stage / stale-lock folders (older than an hour), under the lock
if [ $LOCKED = 1 ]; then
    find "${D:-/}" -maxdepth 1 -type d -name '.make-cert.*' ! -name '.make-cert.lock' -mmin +60 \
        -exec rm -rf {} + 2>/dev/null </dev/null
fi

# --- what is wanted: scope, host name, names, addresses ----------------------
lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
trim() { printf '%s' "$1" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'; }
nodots() { local s=$1; while :; do case "$s" in *.) s=${s%.} ;; *) break ;; esac; done; printf '%s' "$s"; }
run_t() {       # run_t <seconds> <command...>: with a time limit where timeout(1) exists
    local t=$1; shift
    if command -v timeout >/dev/null 2>&1; then timeout "$t" "$@"; else "$@"; fi
}
# a DNS label: 1-63 of a-z 0-9 -, not starting or ending with - (as tlscert's valid_label)
valid_label() {
    case "$1" in ""|-*|*-|*[!a-z0-9-]*) return 1 ;; esac
    [ ${#1} -le 63 ]
}
# a DNS name: such labels joined by dots, 253 characters at most
valid_dns() {
    local rest=$1 l
    [ -n "$1" ] && [ ${#1} -le 253 ] || return 1
    while :; do
        l=${rest%%.*}
        valid_label "$l" || return 1
        [ "$l" = "$rest" ] && return 0
        rest=${rest#*.}
    done
}
# an IPv4 address as inet_pton parses it: four decimal parts 0-255, no
# leading zero (as tlscert's is_ipv4)
is_ipv4() {
    local rest=$1 p i=0
    case "$1" in ""|*[!0-9.]*|.*|*.|*..*) return 1 ;; esac
    while :; do
        p=${rest%%.*}
        case "$p" in 0?*|????*) return 1 ;; esac
        [ "$p" -le 255 ] || return 1
        i=$((i + 1))
        [ "$p" = "$rest" ] && break
        rest=${rest#*.}
    done
    [ $i = 4 ]
}

# cert_options (fine rule 5): lines "scope = VALUE", spaces around = allowed,
# key and value in any case; the last such line wins; "wide" is wide,
# anything else narrow - as kraken_doa reads it (tlscert::read_scope_wide)
SCOPE=narrow; NEW_CA_REQ=0
if [ -f "$OPTS" ]; then
    # fine rule 29: "new_ca = 1" (the page's New certificate authority)
    v=$(sed -n 's/^[[:space:]]*[Nn][Ee][Ww]_[Cc][Aa][[:space:]]*=//p' "$OPTS" 2>/dev/null | tail -n 1)
    if [ "$(trim "$v")" = 1 ]; then NEW_CA_REQ=1; fi
    v=$(sed -n 's/^[[:space:]]*[Ss][Cc][Oo][Pp][Ee][[:space:]]*=//p' "$OPTS" 2>/dev/null | tail -n 1)
    v=$(lower "$(trim "$v")")
    case "$v" in
    wide) SCOPE=wide ;;
    narrow) ;;
    *) note "cert_options has no scope=narrow / scope=wide line - narrow scope" ;;
    esac
fi

# This machine's IPv4 addresses on up interfaces that aren't the loopback,
# one per line (127/8 and link-local 169.254/16 left out) - getifaddrs' view
local_ipv4s() {
    local out=""
    if command -v ip >/dev/null 2>&1; then
        out=$(ip -4 -o addr show up 2>/dev/null </dev/null |
              awk '$2 != "lo" { for (i = 1; i < NF; i++) if ($i == "inet") { a = $(i + 1); sub(/\/.*/, "", a); print a; break } }')
    fi
    if [ -z "$out" ] && command -v ifconfig >/dev/null 2>&1; then
        if [ "$OS" = Darwin ]; then out=$(ifconfig -u 2>/dev/null </dev/null); else out=$(ifconfig 2>/dev/null </dev/null); fi
        out=$(printf '%s\n' "$out" | awk '/^[^ \t]/ { lo = ($0 ~ /LOOPBACK|Loopback/) }
                                           $1 == "inet" && !lo { a = $2; sub(/^addr:/, "", a); print a }')
    fi
    if [ -z "$out" ] && [ "$OS" != Darwin ]; then
        out=$(hostname -I 2>/dev/null </dev/null | tr ' ' '\n')
    fi
    printf '%s\n' "$out" | awk -F. 'NF == 4 && $1 != 127 && !($1 == 169 && $2 == 254)'
}
# stdin: IPv4 addresses -> the ones inside the wide scope's private ranges
# (10/8, 172.16/12, 192.168/16, 100.64/10), each once. A public address is
# left out: a name outside the CA's constraints would fail the whole
# certificate
private_v4() {
    awk -F. 'NF == 4 && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ &&
             $1 + 0 <= 255 && $2 + 0 <= 255 && $3 + 0 <= 255 && $4 + 0 <= 255 &&
             ($1 == 10 || ($1 == 172 && $2 >= 16 && $2 <= 31) || ($1 == 192 && $2 == 168) ||
              ($1 == 100 && $2 >= 64 && $2 <= 127)) {
                 a = ($1 + 0) "." ($2 + 0) "." ($3 + 0) "." ($4 + 0); if (!seen[a]++) print a }'
}

# The name avahi announces: after an mDNS name conflict it is not hostname's
# (the second "krakensdr" on a network becomes "krakensdr-2"). Fine rule 7:
# busctl, then gdbus (2 s timeouts each, once avahi's GetState has settled -
# waited for at most 3 s, through busctl or else gdbus), then avahi-resolve
# on a private IPv4 address (only a .local answer). Quietly tries what
# exists; prints nothing when none answers.
avahi_host() {
    local h="" s i a bus=0 gd=0
    command -v busctl >/dev/null 2>&1 && bus=1
    command -v gdbus >/dev/null 2>&1 && gd=1
    # still registering (1) or resolving a conflict (3): let it settle (2 = running)
    i=0
    while [ $i -lt 6 ]; do
        if [ $bus = 1 ]; then
            s=$(run_t 3 busctl --timeout=2 call org.freedesktop.Avahi / org.freedesktop.Avahi.Server GetState 2>/dev/null </dev/null) || break
        elif [ $gd = 1 ]; then
            s=$(run_t 3 gdbus call --system --timeout 2 --dest org.freedesktop.Avahi --object-path / \
                    --method org.freedesktop.Avahi.Server.GetState 2>/dev/null </dev/null) || break
        else
            break
        fi
        case "$s" in "i 1"|"i 3"|"(1,)"|"(3,)") sleep 0.5 2>/dev/null || sleep 1 ;; *) break ;; esac
        i=$((i + 1))
    done
    if [ $bus = 1 ]; then
        h=$(run_t 3 busctl --timeout=2 call org.freedesktop.Avahi / org.freedesktop.Avahi.Server GetHostName 2>/dev/null </dev/null |
            sed -n 's/^s "\(.*\)"$/\1/p')
    fi
    if [ -z "$h" ] && [ $gd = 1 ]; then
        h=$(run_t 3 gdbus call --system --timeout 2 --dest org.freedesktop.Avahi --object-path / \
                --method org.freedesktop.Avahi.Server.GetHostName 2>/dev/null </dev/null |
            sed -n "s/^('\(.*\)',)\$/\1/p")
    fi
    if [ -z "$h" ] && command -v avahi-resolve >/dev/null 2>&1; then
        a=$(local_ipv4s | private_v4 | head -n 1)
        if [ -n "$a" ]; then
            h=$(run_t 3 avahi-resolve -a "$a" 2>/dev/null </dev/null | awk 'NF >= 2 { print $2; exit }')
            h=$(nodots "$h")
            case "$h" in *.local) h=${h%.local} ;; *) h="" ;; esac   # an mDNS answer only
        fi
    fi
    printf '%s' "$h"
}

# fine rule 24: the host name is the system's (KRAKEN_CERT_HOST, macOS'
# LocalHostName, hostname); the name avahi announces - another after an mDNS
# conflict, krakensdr-2 - is an EXTRA name: a conflict that comes and goes
# never makes a new CA
AVAHI_RAW=""
if [ -n "${KRAKEN_CERT_HOST:-}" ]; then
    RAW=$(trim "$KRAKEN_CERT_HOST")         # as it is: a dot makes it unusable
else
    RAW=""
    if [ "$OS" = Darwin ]; then
        RAW=$(scutil --get LocalHostName 2>/dev/null </dev/null) || RAW=""
    else
        AVAHI_RAW=$(avahi_host)
        AVAHI_RAW=$(trim "${AVAHI_RAW%%.*}")
    fi
    [ -n "$RAW" ] || RAW=$(hostname -s 2>/dev/null </dev/null) || RAW=""
    [ -n "$RAW" ] || RAW=$(hostname 2>/dev/null </dev/null) || RAW=""
    [ -n "$RAW" ] || RAW=$(uname -n 2>/dev/null) || RAW=""
    RAW=$(trim "${RAW%%.*}")
fi
HOST=$(lower "$RAW")
AVAHI=$(lower "$AVAHI_RAW")
if ! valid_label "$AVAHI" || [ "$AVAHI" = "$HOST" ]; then AVAHI=""; fi
# not a DNS label: then only localhost (and in wide scope the addresses)
if valid_label "$HOST"; then USABLE=1; CN=$HOST; else USABLE=0; CN=localhost; fi
[ $USABLE = 1 ] || note "the host name '$RAW' is not usable in DNS - the certificate names only localhost"

NAMES=" "
add_name() { case "$NAMES" in *" $1 "*) ;; *) NAMES="$NAMES$1 " ;; esac; }
if [ $USABLE = 1 ]; then
    add_name "$HOST"
    add_name "$HOST.local"
fi
if [ -n "$AVAHI" ]; then
    add_name "$AVAHI"
    add_name "$AVAHI.local"
fi
if [ $USABLE = 1 ] && [ -r "$RESOLV" ]; then
    # fine rule 30: the LAST search line (what glibc uses) and the domain
    # line, and of them only the suffixes the Host allow-list trusts; any
    # other domain only through KRAKEN_CERT_NAMES
    for d in $(awk '$1 == "search" { s = ""; for (i = 2; i <= NF; i++) s = s " " $i }
                    $1 == "domain" { m = ""; for (i = 2; i <= NF; i++) m = m " " $i }
                    END { print s " " m }' "$RESOLV" 2>/dev/null); do
        d=$(nodots "$(lower "$d")")
        case "$d" in lan|home|localdomain|home.arpa) add_name "$HOST.$d" ;; esac
    done
fi
add_name localhost
# KRAKEN_CERT_NAMES (fine rule 4): split on commas and white space,
# lowercased, trailing dots off; an address (browsers never match one
# against a DNS name) and what is no DNS name are left out, each with a note
if [ -n "${KRAKEN_CERT_NAMES:-}" ]; then
    for n in $(printf '%s' "$KRAKEN_CERT_NAMES" | tr ',\r\v\f' '    '); do
        n=$(nodots "$(lower "$n")")
        [ -n "$n" ] || continue
        if is_ipv4 "$n"; then
            note "KRAKEN_CERT_NAMES: $n is an address - left out (browsers never match an address against a DNS name; wide scope names the receiver's private addresses)"
        elif ! valid_dns "$n"; then
            case "$n" in
            *:*) case "$n" in *[!0-9a-f:.]*) ;; *)
                     note "KRAKEN_CERT_NAMES: $n is an address - left out (browsers never match an address against a DNS name)"
                     continue ;;
                 esac ;;
            esac
            note "KRAKEN_CERT_NAMES: '$n' is not a DNS name - left out"
        else
            add_name "$n"
        fi
    done
fi
IPS="127.0.0.1 ::1"
if [ $SCOPE = wide ]; then
    for a in $(local_ipv4s | private_v4); do IPS="$IPS $a"; done
fi
# the host's OWN names (fine rule 18): one of them outside the CA is a rename
# - a new CA; any other name outside it is left out of the server certificate
OWN=" "
[ $USABLE = 0 ] || OWN=" $HOST $HOST.local "
# a new CA's permitted DNS subtrees: every wanted name
NC_DNS=$NAMES
[ $SCOPE = narrow ] || NC_DNS="${NAMES}local lan "

# --- reading certificates -----------------------------------------------------
# info <cert file> <days>: one openssl call -> I_OK, I_SUBJ, I_ISSUER (RFC 2253),
# I_PUB (its public key), I_VALID (still valid in <days> days; judged by the
# message: OpenSSL 3.0.2 exits 0 for "will expire" too), I_TEXT (x509 -text)
# I_NB its notBefore (Unix seconds), I_FP with "fp" its SHA-256 fingerprint (OpenSSL)
info() {
    local fp=""
    [ "${3:-}" != fp ] || fp=$FP_OPT
    I_TEXT=$(ossl x509 -in "$1" -noout -nameopt RFC2253 -subject -issuer -pubkey $fp -checkend $(($2 * 86400)) -text 2>/dev/null)
    I_NB=$(printf '%s\n' "$I_TEXT" | nb_epoch)
    I_FP=$(printf '%s\n' "$I_TEXT" | sed -n 's/^[Ss][Hh][Aa]256 [Ff]ingerprint=//p' | head -n 1)
    I_SUBJ=$(printf '%s\n' "$I_TEXT" | sed -n 's/^subject= *//p' | head -n 1)
    I_ISSUER=$(printf '%s\n' "$I_TEXT" | sed -n 's/^issuer= *//p' | head -n 1)
    I_PUB=$(printf '%s\n' "$I_TEXT" | sed -n '/-----BEGIN PUBLIC KEY-----/,/-----END PUBLIC KEY-----/p')
    case "$I_TEXT" in *"will not expire"*) I_VALID=1 ;; *) I_VALID=0 ;; esac
    if [ -n "$I_SUBJ" ] && [ -n "$I_PUB" ]; then I_OK=1; else I_OK=0; fi
}
# key_pub <file>: its public key - only for a key uSockets can load: PEM,
# unencrypted (fine rule 27; a DER or encrypted key prints nothing)
key_pub() {
    grep -q -e 'ENCRYPTED PRIVATE KEY' -e '4,ENCRYPTED' "$1" 2>/dev/null && return 0
    ossl pkey -inform PEM -in "$1" -passin pass: -pubout 2>/dev/null
}
# nb_epoch (stdin: x509 -text): the first "Not Before: Mon DD HH:MM:SS YYYY GMT" in Unix seconds
nb_epoch() {
    awk '/Not Before *:/ { s = $0; sub(/.*Not Before *: */, "", s); n = split(s, f, " ")
            m = index("JanFebMarAprMayJunJulAugSepOctNovDec", f[1]); if (m == 0 || n < 4) exit
            m = (m + 2) / 3; split(f[3], t, ":"); y = f[4] + 0; d = f[2] + 0
            if (m <= 2) { y--; mm = m + 9 } else mm = m - 3
            era = int(y / 400); yoe = y - era * 400
            doy = int((153 * mm + 2) / 5) + d - 1; doe = yoe * 365 + int(yoe / 4) - int(yoe / 100) + doy
            printf "%.0f\n", (era * 146097 + doe - 719468) * 86400 + t[1] * 3600 + t[2] * 60 + t[3]; exit }'
}
# ca_fp <file>: the SHA-256 fingerprint of its first certificate, AB:CD:... (fine rule 26)
ca_fp() {
    if [ -n "$FP_OPT" ]; then
        ossl x509 -in "$1" -noout $FP_OPT 2>/dev/null | sed -n 's/^[Ss][Hh][Aa]256 [Ff]ingerprint=//p'
    else
        "$OSSL" x509 -in "$1" -outform DER 2>/dev/null </dev/null | "$OSSL" dgst -sha256 2>/dev/null |
            sed 's/.*= *//' | tr 'abcdef' 'ABCDEF' | sed 's/\(..\)/\1:/g; s/:$//'
    fi
}
# clock_synced (fine rule 23): Linux - timedatectl says NTPSynchronized=yes
# (the kernel's adjtimex state; without timedatectl: not known, no); others yes
clock_synced() {
    [ "$OS" = Linux ] || return 0
    [ "$(run_t 3 timedatectl show -p NTPSynchronized --value 2>/dev/null </dev/null)" = yes ]
}
# subjectAltName of x509 -text (stdin) as sorted "DNS:x" / "IP:a" lines (::1
# is printed in full); DNS names lowercased - compared case-insensitively, as
# tlscert does. Any other kind of entry stays as printed: never the wanted set
san_of() {
    awk '/X509v3 Subject Alternative Name/ { getline; n = split($0, p, ",")
            for (i = 1; i <= n; i++) { s = p[i]; gsub(/^[ \t]+|[ \t]+$/, "", s); sub(/^IP Address:/, "IP:", s)
                                       if (s ~ /^DNS:/) s = "DNS:" tolower(substr(s, 5)); print s }
            exit }' | sed 's/^IP:0:0:0:0:0:0:0:1$/IP:::1/' | LC_ALL=C sort -u
}
# the permitted subtrees of a CA's name constraints in x509 -text (stdin),
# "DNS:x" / "IP:a/mask" per line
permitted_of() {
    awk '/X509v3 Name Constraints/ { inside = 1; next }
        !inside { next }
        /^[ \t]*Permitted:/ { perm = 1; next }
        /^[ \t]*Excluded:/ { perm = 0; next }
        /^[ \t]*(DNS|IP|email|URI|DirName|othername|Registered ID):/ { if (perm) { s = $0; gsub(/^[ \t]+|[ \t]+$/, "", s); print s }; next }
        { exit }'
}
# dns_permitted <name> <DNS constraints, space-separated>: RFC 5280 / OpenSSL's
# nc_dns - the name itself or anything under it (".x": only under it). No DNS
# constraint at all = every name (such a CA is replaced: fine rule 18).
dns_permitted() {
    local c any=0
    for c in $2; do
        any=1
        case "$c" in
        .*) case "$1" in *"$c") return 0 ;; esac ;;
        *)  [ "$1" = "$c" ] && return 0
            case "$1" in *".$c") return 0 ;; esac ;;
        esac
    done
    [ $any = 0 ]
}
# verifies <cert> <ca> [notime]: issued by that CA, a server certificate inside
# its constraints - valid now, unless "notime"
verifies() {
    local t=""
    [ "${3:-}" != notime ] || t=$V_NOTIME
    ossl verify $V_ONLY $t -CAfile "$2" -purpose sslserver "$1" >/dev/null 2>&1
}
# self_signed <pem> (fine rule 15): self-issued (I_SUBJ = I_ISSUER of the last
# info) and its signature verifies with its own key. The signature is checked
# by itself (asn1parse + dgst): openssl verify would judge its extensions and
# key usage too (build_page.py's has no keyCertSign). RSA PKCS#1 and ECDSA
# with SHA-1 / SHA-2, as tlscert's self_signed
self_signed() {
    local md
    [ "$L_SUBJ" = "$L_ISSUER" ] || return 1
    ossl x509 -in "$1" -outform DER -out "$WORK/ss.der" 2>/dev/null || return 1
    # tbsCertificate (offset, length with its header), signatureAlgorithm, the signature's bytes
    set -- $(ossl asn1parse -inform DER -in "$WORK/ss.der" 2>/dev/null | awk '
        match($0, /^ *[0-9]+:d=[0-9]+ +hl= *[0-9]+ +l= *[0-9]+/) {
            s = substr($0, RSTART, RLENGTH); off = s; sub(/:.*/, "", off)
            d = s; sub(/.*:d=/, "", d); sub(/ .*/, "", d)
            hl = s; sub(/.*hl= */, "", hl); sub(/ .*/, "", hl)
            l = s; sub(/.* l= */, "", l)
            if (d + 0 == 1) { n++; o[n] = off + 0; h[n] = hl + 0; len[n] = l + 0 }
            if (d + 0 == 2 && n == 2 && alg == "" && $0 ~ /OBJECT/) { a = $0; sub(/.*:/, "", a); gsub(/[ \t]/, "", a); alg = a }
        }
        END { if (n == 3 && alg != "") print o[1], h[1] + len[1], o[3] + h[3] + 1, len[3] - 1, alg }')
    [ $# = 5 ] || return 1
    case "$5" in
    sha1WithRSAEncryption|ecdsa-with-SHA1) md=-sha1 ;;
    sha256WithRSAEncryption|ecdsa-with-SHA256) md=-sha256 ;;
    sha384WithRSAEncryption|ecdsa-with-SHA384) md=-sha384 ;;
    sha512WithRSAEncryption|ecdsa-with-SHA512) md=-sha512 ;;
    *) return 1 ;;
    esac
    dd if="$WORK/ss.der" of="$WORK/ss.tbs" bs=1 skip=$1 count=$2 2>/dev/null &&
        dd if="$WORK/ss.der" of="$WORK/ss.sig" bs=1 skip=$3 count=$4 2>/dev/null || return 1
    printf '%s\n' "$L_PUB" > "$WORK/ss.pub"
    ossl dgst $md -verify "$WORK/ss.pub" -signature "$WORK/ss.sig" "$WORK/ss.tbs" 2>/dev/null | grep -q 'Verified OK'
}
# kraken_cn (stdin: CN values, one per line): one starts "KrakenSDR " and
# contains " CA " - the name of a CA of ours (fine rule 22)
kraken_cn() {
    local cn
    while IFS= read -r cn; do
        case "$cn" in "KrakenSDR "*) case "$cn" in *" CA "*) return 0 ;; esac ;; esac
    done
    return 1
}
# kraken_issued <subject> <issuer> (RFC 2253): both with O=KrakenSDR receiver,
# the issuer's CN that of a CA of ours (fine rule 22: a CA that was deleted,
# or replaced by a copied one)
kraken_issued() {
    case ",$1," in *",O=KrakenSDR receiver,"*) ;; *) return 1 ;; esac
    case ",$2," in *",O=KrakenSDR receiver,"*) ;; *) return 1 ;; esac
    printf '%s\n' "$2" | tr ',' '\n' | sed -n 's/^CN=//p' | kraken_cn
}
# from_retired <pem>: issued (no time check) by the first certificate of one
# of the folder's krakensdr-ca.*.retired.crt (fine rule 22)
from_retired() {
    local f n=0
    set +f
    for f in "$D"/krakensdr-ca.*.retired.crt; do
        set -f
        [ -f "$f" ] || continue
        n=$((n + 1))
        [ "$(blocks "$f" "$WORK/rt$n.")" -ge 1 ] 2>/dev/null || continue
        if verifies "$1" "$WORK/rt$n.1.pem" notime; then return 0; fi
    done
    set -f
    return 1
}
# blocks <file> <prefix>: its PEM certificates as <prefix>1.pem, 2, ... -> how many
blocks() {
    awk -v p="$2" '/-----BEGIN CERTIFICATE-----/ { n++; f = p n ".pem" }
        f != "" { print > f }
        /-----END CERTIFICATE-----/ { if (f != "") close(f); f = "" }
        END { print n + 0 }' "$1" 2>/dev/null || echo 0
}
b64() { sed '/-----/d' "$1" 2>/dev/null | tr -d ' \t\r\n'; }   # a PEM block's base64, for comparing
# fix_mode <file>: a key left readable by others is set to 0600 again (fine
# rule 13); a symlink (not ours) is left alone
fix_mode() {
    [ -f "$1" ] && [ ! -L "$1" ] || return 0
    case "$(ls -l "$1" 2>/dev/null)" in "-rw-------"*) ;; *) chmod 600 "$1" 2>/dev/null ;; esac
}
# dated_name <prefix> <suffix>: <prefix>YYYYMMDD<suffix> (UTC), then -2, -3 ...
# before the suffix, by which names are taken in <dir> (fine rules 1, 2, 12, 15)
dated_name() {
    local day n i=2
    day=$(date -u +%Y%m%d); n="$1$day$2"
    while [ -e "$D/$n" ] || [ -L "$D/$n" ]; do n="$1$day-$i$2"; i=$((i + 1)); done
    printf '%s' "$n"
}
not_managed() {     # <reason>: left alone - as long as the pair is there
    [ -s "$CRT" ] && [ -s "$KEY" ] ||
        fail "$1, and server.crt / server.key is missing - nothing was changed"
    echo "reason: $1"
    echo not-managed
    exit 0
}

# --- managed or not -----------------------------------------------------------
if [ -L "$CRT" ] || [ -L "$KEY" ]; then not_managed "server.crt or server.key is a symbolic link"; fi
# fine rule 12: a file this user may not read (another user's - made by `sudo
# ./run.sh`?) is a failure, never a reason for a new key or CA
for f in "$CRT" "$KEY" "$CA_CRT" "$CA_KEY"; do
    if [ -e "$f" ] && [ ! -r "$f" ]; then
        fail "${f##*/} cannot be read by $(id -un 2>/dev/null || echo this user) - check its owner - nothing was changed"
    fi
done
# the CA certificate: its first certificate counts (as tlscert's)
CA_NB=0
[ ! -s "$CA_CRT" ] || CA_NB=$(blocks "$CA_CRT" "$WORK/cablk")
HAVE_CRT=0; OURS=0; LEGACY=0; PRIOR=0; L_CN=""
if [ -s "$CRT" ]; then
    # fine rule 11: a server.crt with no certificate in it is not replaced blindly
    nb=$(blocks "$CRT" "$WORK/blk")
    [ "$nb" -ge 1 ] 2>/dev/null ||
        fail "server.crt holds no certificate - nothing was changed (move it away: a new one is made then; or replace it)"
    info "$WORK/blk1.pem" 60
    [ $I_OK = 1 ] ||
        fail "server.crt holds no certificate openssl can read - nothing was changed (move it away: a new one is made then; or replace it)"
    HAVE_CRT=1
    L_SUBJ=$I_SUBJ; L_ISSUER=$I_ISSUER; L_PUB=$I_PUB; L_VALID=$I_VALID; L_NB=$I_NB
    L_SAN=$(printf '%s\n' "$I_TEXT" | san_of)
    # its CN (RFC 2253 prints the subject last entry first: the last CN= is
    # the first in the certificate, as tlscert's name_entry reads it)
    L_CN=$(printf '%s\n' "$L_SUBJ" | tr ',' '\n' | sed -n 's/^CN=//p' | tail -n 1)
    # issued by our CA: chain and signature, no time check (fine rule 10)
    if [ "$CA_NB" -ge 1 ] && verifies "$WORK/blk1.pem" "$WORK/cablk1.pem" notime; then
        OURS=1
    else
        # fine rule 15: PR #5's (O=KrakenSDR receiver) and the older install.sh
        # / Docker / Windows one (exactly CN=krakensdr) - only when self-signed
        legacy=0
        if [ "$L_SUBJ" = "CN=krakensdr" ]; then legacy=1
        else case ",$L_SUBJ," in *",O=KrakenSDR receiver,"*) legacy=1 ;; esac; fi
        if [ $legacy = 1 ] && self_signed "$WORK/blk1.pem"; then LEGACY=1
        # fine rule 22: ours from a CA of ours that was replaced or deleted
        elif kraken_issued "$L_SUBJ" "$L_ISSUER" || from_retired "$WORK/blk1.pem"; then PRIOR=1; fi
    fi
    [ $OURS = 1 ] || [ $LEGACY = 1 ] || [ $PRIOR = 1 ] ||
        not_managed "server.crt is not a KrakenSDR certificate ($L_SUBJ, issued by $L_ISSUER)"
fi

# --- the keys: kept whatever their type ---------------------------------------
# One that can be read but not used (a passphrase, garbage) is never deleted
# or overwritten (fine rule 12): it is moved aside and a new key is made
KEY_NEW=0; K_PUB=""; ASIDE=""
if [ -s "$KEY" ]; then
    K_PUB=$(key_pub "$KEY")
    case "$K_PUB" in
    *"BEGIN PUBLIC KEY"*) ;;
    *)  KEY_NEW=1; K_PUB=""
        ASIDE=$(dated_name server.key.unreadable- "") ;;
    esac
else
    KEY_NEW=1
fi
CA_KPUB=""; CA_ASIDE=""
if [ -s "$CA_KEY" ]; then
    CA_KPUB=$(key_pub "$CA_KEY")
    case "$CA_KPUB" in
    *"BEGIN PUBLIC KEY"*) ;;
    *)  CA_KPUB=""; CA_ASIDE=$(dated_name krakensdr-ca.key.unreadable- "") ;;
    esac
fi

# --- the CA -------------------------------------------------------------------
# fine rule 31: no krakensdr-ca.key beside a valid CA certificate (the key
# taken offline) - kept while nothing needs signing
KEY_AWAY=0
[ -s "$CA_KEY" ] || [ -n "$CA_ASIDE" ] || KEY_AWAY=1
CA_OK=0; C_TEXT=""; C_NB=""; CA_FP=""; CLOCK_SUSPECT=0
if [ "$CA_NB" -ge 1 ] && { [ -n "$CA_KPUB" ] || [ $KEY_AWAY = 1 ]; }; then
    info "$WORK/cablk1.pem" 365 fp
    C_TEXT=$I_TEXT; C_VALID=$I_VALID; C_NB=$I_NB; CA_FP=$I_FP
    if [ $I_OK = 1 ] && { [ $KEY_AWAY = 1 ] || [ "$I_PUB" = "$CA_KPUB" ]; }; then
        case "$C_TEXT" in *"CA:TRUE"*) CA_OK=1 ;; esac
    fi
fi
NEED_CA=0; REASON=""; NOT_COVERED=""; nc=""
if [ $new_ca = 1 ]; then
    NEED_CA=1; REASON="--new-ca"
elif [ $NEW_CA_REQ = 1 ]; then
    NEED_CA=1; REASON="asked for on the page (new_ca=1 in cert_options)"
elif [ -n "$CA_ASIDE" ]; then
    NEED_CA=1; REASON="krakensdr-ca.key could not be read (moved aside to $CA_ASIDE)"
elif [ $CA_OK = 0 ]; then
    NEED_CA=1
    if [ -e "$CA_CRT" ] || [ -e "$CA_KEY" ]; then REASON="the CA key is missing or does not match"
    elif [ $PRIOR = 1 ]; then REASON="the CA that issued server.crt is gone"
    elif [ $HAVE_CRT = 1 ]; then REASON="replaces the self-signed certificate"
    else REASON="first certificate"; fi
elif [ $C_VALID = 0 ]; then
    NEED_CA=1; REASON="the old one expires within a year"
else
    # the CA's scope (fine rule 3): wide when its permitted IP subtrees include
    # 10.0.0.0/8, narrow when it has name constraints without that, none
    # without name constraints - never kept (it could vouch for any name)
    perm=$(printf '%s\n' "$C_TEXT" | permitted_of)
    nc=$(printf '%s\n' "$perm" | sed -n 's/^DNS://p' | tr '[:upper:]' '[:lower:]' | tr '\n' ' ')
    case "$C_TEXT" in
    *"X509v3 Name Constraints"*)
        if printf '%s\n' "$perm" | grep -q -x 'IP:10\.0\.0\.0/255\.0\.0\.0'; then ca_scope=wide; else ca_scope=narrow; fi ;;
    *)  ca_scope=none ;;
    esac
    # fine rule 16: limited to TLS servers - extendedKeyUsage exactly serverAuth
    ca_eku=$(printf '%s\n' "$C_TEXT" | awk '/X509v3 Extended Key Usage/ { getline; gsub(/^[ \t]+|[ \t]+$/, ""); print; exit }')
    if [ $ca_scope = none ]; then
        NEED_CA=1; REASON="the CA has no name constraints"
    elif ! printf '%s\n' "$perm" | grep -q '^DNS:' || ! printf '%s\n' "$perm" | grep -q '^IP:'; then
        # fine rule 18: partial constraints leave DNS names (or addresses) open
        NEED_CA=1; REASON="the CA's name constraints have no DNS or no IP subtree"
    elif [ "$ca_eku" != "TLS Web Server Authentication" ]; then
        NEED_CA=1; REASON="the CA is not limited to TLS servers (extendedKeyUsage)"
    elif [ "$ca_scope" != "$SCOPE" ]; then
        NEED_CA=1; REASON="scope $ca_scope -> $SCOPE"
    else
        # fine rule 23: a CA starting more than a day after the clock - a new
        # one only when the clock is known to be right (a Pi without a clock
        # boots with an old time: that must never make every device reinstall)
        if [ -n "$C_NB" ] && [ "$C_NB" -gt $((NOW + 86400)) ]; then
            if clock_synced; then NEED_CA=1; REASON="the CA starts more than a day after the (synchronized) clock"
            else CLOCK_SUSPECT=1
                note "the CA starts more than a day after this clock, which is not known to be synchronized - kept (clock_suspect)"
            fi
        fi
        if [ $NEED_CA = 0 ]; then
            for n in $NAMES; do
                dns_permitted "$n" "$nc" && continue
                case "$OWN" in *" $n "*) NEED_CA=1; REASON="new name $n"; break ;; esac
                NOT_COVERED="$NOT_COVERED $n"
            done
        fi
    fi
fi
[ $NEED_CA = 0 ] || NOT_COVERED=""
# the server certificate's names: the wanted ones the CA covers (fine rule
# 18) and every EXACT name the kept CA permits (fine rule 35: not the wide
# scope's subtrees local / lan) - a search domain gone for a minute (a DHCP
# lease renewed) doesn't take <host>.lan out; a new CA takes every name in.
# The set wanted, one "DNS:x" / "IP:a" per line, sorted
LEAF_NAMES=""
for n in $NAMES; do case "$NOT_COVERED " in *" $n "*) ;; *) LEAF_NAMES="$LEAF_NAMES $n" ;; esac; done
if [ $NEED_CA = 0 ]; then
    for n in $nc; do
        case "$n" in local|lan) continue ;; esac
        valid_dns "$n" || continue
        case "$LEAF_NAMES " in *" $n "*) ;; *) LEAF_NAMES="$LEAF_NAMES $n" ;; esac
    done
fi
WANT_SAN=$( { for n in $LEAF_NAMES; do echo "DNS:$n"; done; for a in $IPS; do echo "IP:$a"; done; } | LC_ALL=C sort -u)
[ -z "$NOT_COVERED" ] ||
    note "outside this receiver's CA, left out of the certificate:$NOT_COVERED (--new-ca takes them in; every device then installs the new CA)"

# --- the server certificate ---------------------------------------------------
ISSUE=$NEED_CA
if [ $ISSUE = 0 ]; then
    ISSUE=1
    if [ $force = 1 ]; then REASON="--force"
    elif [ $HAVE_CRT = 0 ]; then REASON="no server certificate"
    elif [ -n "$ASIDE" ]; then REASON="server.key could not be read (moved aside to $ASIDE) - a new server key"
    elif [ $KEY_NEW = 1 ]; then REASON="a new server key"
    elif [ $PRIOR = 1 ]; then REASON="issued by an earlier CA of this receiver - now from the current one"
    elif [ $OURS = 0 ]; then REASON="replaces the self-signed certificate"
    elif [ "$L_PUB" != "$K_PUB" ]; then REASON="server.crt does not match server.key"
    elif [ "$L_SAN" != "$WANT_SAN" ]; then
        if [ "$(printf '%s\n' "$L_SAN" | grep '^DNS:')" = "$(printf '%s\n' "$WANT_SAN" | grep '^DNS:')" ]; then
            REASON="the addresses changed"
        else
            REASON="the names changed"
        fi
    elif [ "$L_CN" != "$CN" ]; then
        # the subject's CN=<host> / localhost: with the CA's names staying in
        # (fine rule 35) a rename inside the CA or an unusable host name can
        # leave the names as they were
        REASON="the names changed"
    elif [ $L_VALID = 0 ]; then REASON="it expires within 60 days"
    elif [ "$nb" != 2 ] || [ "$(b64 "$WORK/blk2.pem")" != "$(b64 "$WORK/cablk1.pem")" ]; then
        REASON="server.crt does not hold the CA certificate after it"
    elif [ -n "$L_NB" ] && [ "$L_NB" -gt $((NOW + 86400)) ]; then
        # fine rule 23: dated from a clock that was ahead - re-issued only when
        # this clock is known to be right (else the new one would be dated
        # from a wrong time), and never a reason for a new CA (also not with
        # the CA key away: kept)
        if ! clock_synced; then
            ISSUE=0
            note "server.crt starts more than a day after this clock, which is not known to be synchronized - kept"
        elif [ $KEY_AWAY = 1 ]; then
            ISSUE=0
            note "server.crt starts more than a day after the clock - kept: krakensdr-ca.key is missing (a clock is never a reason for a new CA)"
        else
            REASON="it starts more than a day after the clock"
        fi
    else ISSUE=0
    fi
fi
# fine rule 31: something to sign and the CA key away - a new CA after all
if [ $ISSUE = 1 ] && [ $NEED_CA = 0 ] && [ $KEY_AWAY = 1 ]; then
    NEED_CA=1; REASON="$REASON, and krakensdr-ca.key is missing"
    NOT_COVERED=""; LEAF_NAMES=$NAMES
fi
if [ $ISSUE = 0 ]; then
    fix_mode "$KEY"
    fix_mode "$CA_KEY"
    [ -n "$CA_FP" ] || CA_FP=$(ca_fp "$WORK/cablk1.pem")
    [ -z "$CA_FP" ] || echo "ca-sha256: $CA_FP"
    [ -z "$NOT_COVERED" ] || echo "not-covered:$NOT_COVERED"
    echo kept
    exit 0
fi

# --- make everything in a folder of <dir>, then move it into place ------------
STAGE=$(mktemp -d "$D/.make-cert.XXXXXX" 2>/dev/null) ||
    fail "cannot write in ${D:-/} (read-only?) - the certificate stays as it was"
START=$((NOW - 3600))       # an hour back, as kraken_doa's generator
# asn1_time <epoch seconds>: YYMMDDHHMMSSZ (UTCTime), YYYYMMDDHHMMSSZ from 2050
asn1_time() {
    awk -v t="$1" 'BEGIN {
        s = t % 86400; z = (t - s) / 86400 + 719468
        era = int(z / 146097); doe = z - era * 146097
        yoe = int((doe - int(doe / 1460) + int(doe / 36524) - int(doe / 146096)) / 365)
        y = yoe + era * 400; doy = doe - (365 * yoe + int(yoe / 4) - int(yoe / 100))
        mp = int((5 * doy + 2) / 153); d = doy - int((153 * mp + 2) / 5) + 1
        m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++
        h = int(s / 3600); mi = int((s % 3600) / 60); se = s % 60
        if (y < 2050) printf "%02d%02d%02d%02d%02d%02dZ\n", y % 100, m, d, h, mi, se
        else printf "%04d%02d%02d%02d%02d%02dZ\n", y, m, d, h, mi, se }'
}
T_START=$(asn1_time $START)
T_END_CA=$(asn1_time $((START + 3650 * 86400)))
T_END_SRV=$(asn1_time $((START + 824 * 86400)))   # fine rule 33
# a new random serial (128 bits) for the next `openssl ca`
serial() {
    local s
    s=$(ossl rand -hex 16 2>/dev/null) || fail "openssl could not make a serial number - nothing was changed"
    printf '%s\n' "$s" > "$STAGE/serial"
}

: > "$STAGE/index.txt"
{
    echo "[ca]"
    echo "default_ca = kca"
    echo "[kca]"
    echo "database = index.txt"
    echo "new_certs_dir = ."
    echo "serial = serial"
    echo "default_md = sha256"
    echo "policy = kpolicy"
    echo "unique_subject = no"
    echo "email_in_dn = no"
    echo "preserve = yes"
    echo "copy_extensions = none"
    echo "[kpolicy]"
    echo "commonName = supplied"
    echo "organizationName = optional"
    echo "[v3_ca]"
    echo "basicConstraints = critical, CA:TRUE, pathlen:0"
    echo "keyUsage = critical, keyCertSign, cRLSign"
    echo "subjectKeyIdentifier = hash"
    echo "extendedKeyUsage = serverAuth"
    echo "nameConstraints = critical, @nc"
    echo "[nc]"
    i=0
    for n in $NC_DNS; do echo "permitted;DNS.$i = $n"; i=$((i + 1)); done
    if [ $SCOPE = narrow ]; then
        echo "permitted;IP.0 = 127.0.0.1/255.255.255.255"
        echo "permitted;IP.1 = ::1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"
    else
        echo "permitted;IP.0 = 10.0.0.0/255.0.0.0"
        echo "permitted;IP.1 = 172.16.0.0/255.240.0.0"
        echo "permitted;IP.2 = 192.168.0.0/255.255.0.0"
        echo "permitted;IP.3 = 100.64.0.0/255.192.0.0"
        echo "permitted;IP.4 = 127.0.0.0/255.0.0.0"
        echo "permitted;IP.5 = fc00::/fe00::"
        echo "permitted;IP.6 = ::1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"
    fi
    echo "[v3_server]"
    echo "basicConstraints = critical, CA:FALSE"
    echo "keyUsage = critical, digitalSignature, keyEncipherment"
    echo "extendedKeyUsage = serverAuth"
    echo "subjectKeyIdentifier = hash"
    echo "authorityKeyIdentifier = keyid:always"
    echo "subjectAltName = @san"
    echo "[san]"
    i=0
    for n in $LEAF_NAMES; do echo "DNS.$i = $n"; i=$((i + 1)); done
    i=0
    for a in $IPS; do echo "IP.$i = $a"; i=$((i + 1)); done
} > "$STAGE/ca.cnf"
# req_cnf <CN>: a request config (no extensions: `ca` adds them)
req_cnf() {
    echo "[req]"
    echo "distinguished_name = dn"
    echo "prompt = no"
    echo "utf8 = yes"
    echo "string_mask = utf8only"
    echo "[dn]"
    echo "CN = $1"
    echo "O = KrakenSDR receiver"
}

if [ $NEED_CA = 1 ]; then
    must "make the CA key" genrsa -out ca.key 3072
    # UTC (fine rule 1); the host part at most 40 characters: a CN is 64 at most (fine rule 17)
    req_cnf "KrakenSDR $(printf '%.40s' "$CN") CA $(date -u +%Y-%m-%d)" > "$STAGE/ca_req.cnf"
    must "make the CA request" req -new -config ca_req.cnf -key ca.key -out ca.csr
    serial
    must "make the CA certificate" ca -batch -notext -config ca.cnf -selfsign -keyfile ca.key -in ca.csr \
        -extensions v3_ca -startdate "$T_START" -enddate "$T_END_CA" -out ca.crt
    USE_CA_KEY=$STAGE/ca.key; USE_CA_CRT=$STAGE/ca.crt
else
    USE_CA_KEY=$CA_KEY; USE_CA_CRT=$CA_CRT
fi
if [ $KEY_NEW = 1 ]; then
    must "make the server key" genrsa -out server.key 2048
    USE_KEY=$STAGE/server.key
else
    USE_KEY=$KEY
fi
req_cnf "$CN" > "$STAGE/server_req.cnf"
must "make the certificate request" req -new -config server_req.cnf -key "$USE_KEY" -out server.csr
serial
must "sign the server certificate" ca -batch -notext -config ca.cnf -cert "$USE_CA_CRT" -keyfile "$USE_CA_KEY" \
    -in server.csr -extensions v3_server -startdate "$T_START" -enddate "$T_END_SRV" -out leaf.crt
must "read the CA certificate" x509 -in "$USE_CA_CRT" -out ca.pem
# the new pair: chain, signature, constraints, purpose - no time check, the
# dates are ours (fine rule 23). Without -no_check_time (LibreSSL) a CA
# starting after this clock can't be checked: then not checked
if [ -n "$V_NOTIME" ] || [ $NEED_CA = 1 ] || [ -z "$C_NB" ] || [ "$C_NB" -le "$NOW" ]; then
    verifies "$STAGE/leaf.crt" "$STAGE/ca.pem" notime ||
        fail "the new server certificate does not verify against the CA - nothing was changed"
fi
info "$STAGE/leaf.crt" 0
[ "$I_PUB" = "$(key_pub "$USE_KEY")" ] ||
    fail "the new server certificate does not match the key - nothing was changed"
cat "$STAGE/leaf.crt" "$STAGE/ca.pem" > "$STAGE/server.crt" ||
    fail "cannot write in ${D:-/} - nothing was changed"

# Into place. A migrated (self-signed) server.crt is copied first (fine rule
# 15). Then each step is a rename in one folder; the order keeps any
# interruption repairable by the next run (a CA key without its certificate,
# a key without its certificate: both are replaced / re-issued then): old CA
# certificate -> retired, unusable CA key -> aside, new CA key, new CA
# certificate, unusable server key -> aside, new server key, server.crt
mv_into() { chmod "$3" "$1" && mv -f "$1" "$2" || fail "cannot replace $2"; }
# cert_blocks <file> <out>: its CERTIFICATE blocks only (fine rule 25: a
# retired / saved copy never holds a key); has_key <file>: a private key in it
cert_blocks() { awk '/-----BEGIN CERTIFICATE-----/ { p = 1 } p { print } /-----END CERTIFICATE-----/ { p = 0 }' "$1" > "$2"; }
has_key() { grep -q 'PRIVATE KEY-----' "$1" 2>/dev/null; }
# keep <file> <name>: its certificate blocks as <name> (0644) - and the file
# itself, holding a key, aside as <file>.unreadable-YYYYMMDD (0600)
KEPT_ASIDE=""
keep() {
    local a
    { cert_blocks "$1" "$STAGE/keep.crt" && chmod 644 "$STAGE/keep.crt" && mv "$STAGE/keep.crt" "$D/$2"; } </dev/null 2>/dev/null ||
        fail "cannot keep a copy of ${1##*/} - nothing was changed"
    if has_key "$1"; then
        a=$(dated_name "${1##*/}.unreadable-" "")
        mv "$1" "$D/$a" </dev/null || fail "cannot move ${1##*/} aside"
        fix_mode "$D/$a"
        KEPT_ASIDE="$KEPT_ASIDE $a"
        note "${1##*/} held a private key - moved aside to $a (0600); $2 holds its certificates only"
    fi
}
SAVED=""
if [ $LEGACY = 1 ]; then
    SAVED=$(dated_name server.crt.replaced- "")
    keep "$CRT" "$SAVED"
fi
RETIRED=""; KEY_RETIRED=""
if [ $NEED_CA = 1 ]; then
    # fine rule 2: only a readable certificate, not a symlink, is retired; its
    # key goes - unless the CA isn't ours (no O=KrakenSDR receiver: a user's
    # own CA put at our names): its key is kept as
    # krakensdr-ca.key.retired-YYYYMMDD (rule 32) - whatever the certificate
    # file is (a symlink too: the key is still krakensdr-ca.key)
    rsubj=""
    if [ -s "$CA_CRT" ]; then
        rsubj=$(ossl x509 -in "$CA_CRT" -noout -subject -nameopt RFC2253 2>/dev/null) || rsubj=""
    fi
    if [ -n "$rsubj" ] && [ ! -L "$CA_CRT" ]; then
        RETIRED=$(dated_name krakensdr-ca. .retired.crt)
        keep "$CA_CRT" "$RETIRED"
    fi
    if [ -n "$rsubj" ]; then
        case ",${rsubj#subject=}," in
        *",O=KrakenSDR receiver,"*) ;;
        *)  if [ -s "$CA_KEY" ] && [ -z "$CA_ASIDE" ]; then
                KEY_RETIRED=$(dated_name krakensdr-ca.key.retired- "")
                mv "$CA_KEY" "$D/$KEY_RETIRED" </dev/null || fail "cannot keep $CA_KEY"
                fix_mode "$D/$KEY_RETIRED"
                note "the replaced CA is not one of a KrakenSDR receiver - its key kept as $KEY_RETIRED"
            fi ;;
        esac
    fi
    if [ -n "$CA_ASIDE" ]; then
        mv "$CA_KEY" "$D/$CA_ASIDE" </dev/null || fail "cannot move $CA_KEY aside"
        fix_mode "$D/$CA_ASIDE"
        note "krakensdr-ca.key could not be read - moved aside to $CA_ASIDE, a new CA is made"
    fi
    mv_into "$STAGE/ca.key" "$CA_KEY" 600       # the old CA's key is gone with this
    mv_into "$STAGE/ca.pem" "$CA_CRT" 644
fi
if [ -n "$ASIDE" ]; then
    mv "$KEY" "$D/$ASIDE" </dev/null || fail "cannot move $KEY aside"
    fix_mode "$D/$ASIDE"
    note "server.key could not be read - moved aside to $ASIDE, a new key is made"
fi
[ $KEY_NEW = 0 ] || mv_into "$STAGE/server.key" "$KEY" 600
mv_into "$STAGE/server.crt" "$CRT" 644
fix_mode "$KEY"         # fine rule 13
fix_mode "$CA_KEY"
# fine rule 29: the request is done
if [ $NEW_CA_REQ = 1 ]; then
    { sed '/^[[:space:]]*[Nn][Ee][Ww]_[Cc][Aa][[:space:]]*=/d' "$OPTS" > "$STAGE/opts" && chmod 644 "$STAGE/opts" &&
      mv -f "$STAGE/opts" "$OPTS"; } </dev/null 2>/dev/null || note "cannot take new_ca=1 out of cert_options"
fi

echo "reason: $REASON"
[ -z "$RETIRED" ] || echo "retired: $RETIRED"
[ -z "$KEY_RETIRED" ] || echo "retired: $KEY_RETIRED"
[ -z "$CA_ASIDE" ] || [ $NEED_CA = 0 ] || echo "moved: $CA_ASIDE"
[ -z "$ASIDE" ] || echo "moved: $ASIDE"
for a in $KEPT_ASIDE; do echo "moved: $a"; done
[ -z "$SAVED" ] || echo "saved: $SAVED"
CA_FP=$(ca_fp "$CA_CRT")
[ -z "$CA_FP" ] || echo "ca-sha256: $CA_FP"
[ -z "$NOT_COVERED" ] || echo "not-covered:$NOT_COVERED"
if [ $NEED_CA = 1 ]; then echo ca-created; else echo issued; fi
exit 0
